/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCacheIRCompiler.h"

#include "jit/CacheIR.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"
#include "proxy/Proxy.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

class AutoStubFrame;

// BaselineCacheIRCompiler compiles CacheIR to BaselineIC native code.
class MOZ_RAII BaselineCacheIRCompiler : public CacheIRCompiler
{
    // Some Baseline IC stubs can be used in IonMonkey through SharedStubs.
    // Those stubs have different machine code, so we need to track whether
    // we're compiling for Baseline or Ion.
    ICStubEngine engine_;

    uint32_t stubDataOffset_;
    bool inStubFrame_;
    bool makesGCCalls_;

    MOZ_MUST_USE bool callVM(MacroAssembler& masm, const VMFunction& fun);

    MOZ_MUST_USE bool callTypeUpdateIC(AutoStubFrame& stubFrame, Register obj, ValueOperand val,
                                       Register scratch, LiveGeneralRegisterSet saveRegs);

    MOZ_MUST_USE bool emitStoreSlotShared(bool isFixed);

  public:
    friend class AutoStubFrame;

    BaselineCacheIRCompiler(JSContext* cx, const CacheIRWriter& writer, ICStubEngine engine,
                            uint32_t stubDataOffset)
      : CacheIRCompiler(cx, writer, Mode::Baseline),
        engine_(engine),
        stubDataOffset_(stubDataOffset),
        inStubFrame_(false),
        makesGCCalls_(false)
    {}

    MOZ_MUST_USE bool init(CacheKind kind);

    JitCode* compile();

    bool makesGCCalls() const { return makesGCCalls_; }

  private:
#define DEFINE_OP(op) MOZ_MUST_USE bool emit##op();
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP

    Address stubAddress(uint32_t offset) const {
        return Address(ICStubReg, stubDataOffset_ + offset);
    }
};

#define DEFINE_SHARED_OP(op) \
    bool BaselineCacheIRCompiler::emit##op() { return CacheIRCompiler::emit##op(); }
    CACHE_IR_SHARED_OPS(DEFINE_SHARED_OP)
#undef DEFINE_SHARED_OP

enum class CallCanGC { CanGC, CanNotGC };

// Instructions that have to perform a callVM require a stub frame. Use
// AutoStubFrame before allocating any registers, then call its enter() and
// leave() methods to enter/leave the stub frame.
class MOZ_RAII AutoStubFrame
{
    BaselineCacheIRCompiler& compiler;
#ifdef DEBUG
    uint32_t framePushedAtEnterStubFrame_;
#endif
    Maybe<AutoScratchRegister> tail;

    AutoStubFrame(const AutoStubFrame&) = delete;
    void operator=(const AutoStubFrame&) = delete;

  public:
    explicit AutoStubFrame(BaselineCacheIRCompiler& compiler)
      : compiler(compiler),
#ifdef DEBUG
        framePushedAtEnterStubFrame_(0),
#endif
        tail()
    {
        // We use ICTailCallReg when entering the stub frame, so ensure it's not
        // used for something else.
        if (compiler.allocator.isAllocatable(ICTailCallReg))
            tail.emplace(compiler.allocator, compiler.masm, ICTailCallReg);
    }

    void enter(MacroAssembler& masm, Register scratch, CallCanGC canGC = CallCanGC::CanGC) {
        if (compiler.engine_ == ICStubEngine::Baseline) {
            EmitBaselineEnterStubFrame(masm, scratch);
#ifdef DEBUG
            framePushedAtEnterStubFrame_ = masm.framePushed();
#endif
        } else {
            EmitIonEnterStubFrame(masm, scratch);
        }

        MOZ_ASSERT(!compiler.inStubFrame_);
        compiler.inStubFrame_ = true;
        if (canGC == CallCanGC::CanGC)
            compiler.makesGCCalls_ = true;
    }
    void leave(MacroAssembler& masm, bool calledIntoIon = false) {
        MOZ_ASSERT(compiler.inStubFrame_);
        compiler.inStubFrame_ = false;

        if (compiler.engine_ == ICStubEngine::Baseline) {
#ifdef DEBUG
            masm.setFramePushed(framePushedAtEnterStubFrame_);
            if (calledIntoIon)
                masm.adjustFrame(sizeof(intptr_t)); // Calls into ion have this extra.
#endif

            EmitBaselineLeaveStubFrame(masm, calledIntoIon);
        } else {
            EmitIonLeaveStubFrame(masm);
        }
    }

#ifdef DEBUG
    ~AutoStubFrame() {
        MOZ_ASSERT(!compiler.inStubFrame_);
    }
#endif
};

bool
BaselineCacheIRCompiler::callVM(MacroAssembler& masm, const VMFunction& fun)
{
    MOZ_ASSERT(inStubFrame_);

    JitCode* code = cx_->jitRuntime()->getVMWrapper(fun);
    if (!code)
        return false;

    MOZ_ASSERT(fun.expectTailCall == NonTailCall);
    if (engine_ == ICStubEngine::Baseline)
        EmitBaselineCallVM(code, masm);
    else
        EmitIonCallVM(code, fun.explicitStackSlots(), masm);
    return true;
}

JitCode*
BaselineCacheIRCompiler::compile()
{
#ifndef JS_USE_LINK_REGISTER
    // The first value contains the return addres,
    // which we pull into ICTailCallReg for tail calls.
    masm.adjustFrame(sizeof(intptr_t));
#endif
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    do {
        switch (reader.readOp()) {
#define DEFINE_OP(op)                   \
          case CacheOp::op:             \
            if (!emit##op())            \
                return nullptr;         \
            break;
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP

          default:
            MOZ_CRASH("Invalid op");
        }

        allocator.nextOp();
    } while (reader.more());

    MOZ_ASSERT(!inStubFrame_);
    masm.assumeUnreachable("Should have returned from IC");

    // Done emitting the main IC code. Now emit the failure paths.
    for (size_t i = 0; i < failurePaths.length(); i++) {
        if (!emitFailurePath(i))
            return nullptr;
        EmitStubGuardFailure(masm);
    }

    Linker linker(masm);
    AutoFlushICache afc("getStubCode");
    Rooted<JitCode*> newStubCode(cx_, linker.newCode<NoGC>(cx_, BASELINE_CODE));
    if (!newStubCode) {
        cx_->recoverFromOutOfMemory();
        return nullptr;
    }

    // All barriers are emitted off-by-default, enable them if needed.
    if (cx_->zone()->needsIncrementalBarrier())
        newStubCode->togglePreBarriers(true, DontReprotect);

    return newStubCode;
}

bool
BaselineCacheIRCompiler::emitGuardShape()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadPtr(addr, scratch);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardGroup()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadPtr(addr, scratch);
    masm.branchTestObjGroup(Assembler::NotEqual, obj, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardProto()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadObjProto(obj, scratch);
    masm.branchPtr(Assembler::NotEqual, addr, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificObject()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.branchPtr(Assembler::NotEqual, addr, obj, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificAtom()
{
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address atomAddr(stubAddress(reader.stubOffset()));

    Label done;
    masm.branchPtr(Assembler::Equal, atomAddr, str, &done);

    // The pointers are not equal, so if the input string is also an atom it
    // must be a different string.
    masm.branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
                      Imm32(JSString::ATOM_BIT), failure->label());

    // Check the length.
    masm.loadPtr(atomAddr, scratch);
    masm.loadStringLength(scratch, scratch);
    masm.branch32(Assembler::NotEqual, Address(str, JSString::offsetOfLength()),
                  scratch, failure->label());

    // We have a non-atomized string with the same length. Call a helper
    // function to do the comparison.
    LiveRegisterSet volatileRegs(RegisterSet::Volatile());
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.loadPtr(atomAddr, scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(str);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, EqualStringsHelper));
    masm.mov(ReturnReg, scratch);

    LiveRegisterSet ignore;
    ignore.add(scratch);
    masm.PopRegsInMaskIgnore(volatileRegs, ignore);
    masm.branchIfFalseBool(scratch, failure->label());

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificSymbol()
{
    Register sym = allocator.useRegister(masm, reader.symbolOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.branchPtr(Assembler::NotEqual, addr, sym, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFixedSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadValue(BaseIndex(obj, scratch, TimesOne), output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadDynamicSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    // We're about to return, so it's safe to clobber obj now.
    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), obj);
    masm.loadValue(BaseIndex(obj, scratch, TimesOne), output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitCallScriptedGetterResult()
{
    MOZ_ASSERT(engine_ == ICStubEngine::Baseline);

    AutoStubFrame stubFrame(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address getterAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegisterExcluding code(allocator, masm, ArgumentsRectifierReg);
    AutoScratchRegister callee(allocator, masm);
    AutoScratchRegister scratch(allocator, masm);

    // First, ensure our getter is non-lazy and has JIT code.
    {
        FailurePath* failure;
        if (!addFailurePath(&failure))
            return false;

        masm.loadPtr(getterAddr, callee);
        masm.branchIfFunctionHasNoScript(callee, failure->label());
        masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
        masm.loadBaselineOrIonRaw(code, code, failure->label());
    }

    allocator.discardStack(masm);

    stubFrame.enter(masm, scratch);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(0);

    // Getter is called with 0 arguments, just |obj| as thisv.
    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));

    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());
    masm.Push(Imm32(0));  // ActualArgc is 0
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::Equal, callee, Imm32(0), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier = cx_->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.movePtr(ImmWord(0), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    stubFrame.leave(masm, true);
    return true;
}

typedef bool (*CallNativeGetterFn)(JSContext*, HandleFunction, HandleObject, MutableHandleValue);
static const VMFunction CallNativeGetterInfo =
    FunctionInfo<CallNativeGetterFn>(CallNativeGetter, "CallNativeGetter");

bool
BaselineCacheIRCompiler::emitCallNativeGetterResult()
{
    AutoStubFrame stubFrame(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address getterAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    stubFrame.enter(masm, scratch);

    // Load the callee in the scratch register.
    masm.loadPtr(getterAddr, scratch);

    masm.Push(obj);
    masm.Push(scratch);

    if (!callVM(masm, CallNativeGetterInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxyGetPropertyFn)(JSContext*, HandleObject, HandleId, MutableHandleValue);
static const VMFunction ProxyGetPropertyInfo =
    FunctionInfo<ProxyGetPropertyFn>(ProxyGetProperty, "ProxyGetProperty");

bool
BaselineCacheIRCompiler::emitCallProxyGetResult()
{
    AutoStubFrame stubFrame(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address idAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    stubFrame.enter(masm, scratch);

    // Load the jsid in the scratch register.
    masm.loadPtr(idAddr, scratch);

    masm.Push(scratch);
    masm.Push(obj);

    if (!callVM(masm, ProxyGetPropertyInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxyGetPropertyByValueFn)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
static const VMFunction ProxyGetPropertyByValueInfo =
    FunctionInfo<ProxyGetPropertyByValueFn>(ProxyGetPropertyByValue, "ProxyGetPropertyByValue");

bool
BaselineCacheIRCompiler::emitCallProxyGetByValueResult()
{
    AutoStubFrame stubFrame(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    stubFrame.enter(masm, scratch);

    masm.Push(idVal);
    masm.Push(obj);

    if (!callVM(masm, ProxyGetPropertyByValueInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadUnboxedPropertyResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    JSValueType fieldType = reader.valueType();
    Address fieldOffset(stubAddress(reader.stubOffset()));
    masm.load32(fieldOffset, scratch);
    masm.loadUnboxedProperty(BaseIndex(obj, scratch, TimesOne), fieldType, output);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardFrameHasNoArgumentsObject()
{
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTest32(Assembler::NonZero,
                      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                      Imm32(BaselineFrame::HAS_ARGS_OBJ),
                      failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameCalleeResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Address callee(BaselineFrameReg, BaselineFrame::offsetOfCalleeToken());
    masm.loadFunctionFromCalleeToken(callee, scratch);
    masm.tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameNumActualArgsResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
    masm.loadPtr(actualArgs, scratch);
    masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadTypedObjectResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    TypedThingLayout layout = reader.typedThingLayout();
    uint32_t typeDescr = reader.typeDescrKey();
    Address fieldOffset(stubAddress(reader.stubOffset()));

    // Get the object's data pointer.
    LoadTypedThingData(masm, layout, obj, scratch1);

    // Get the address being written to.
    masm.load32(fieldOffset, scratch2);
    masm.addPtr(scratch2, scratch1);

    Address fieldAddr(scratch1, 0);
    emitLoadTypedObjectResultShared(fieldAddr, scratch2, layout, typeDescr, output);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameArgumentResult()
{
    AutoOutputRegister output(*this);
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Bounds check.
    masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs()), scratch);
    masm.branch32(Assembler::AboveOrEqual, index, scratch, failure->label());

    // Load the argument.
    masm.loadValue(BaseValueIndex(BaselineFrameReg, index, BaselineFrame::offsetOfArg(0)),
                   output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadEnvironmentFixedSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    BaseIndex slot(obj, scratch, TimesOne);

    // Check for uninitialized lexicals.
    masm.branchTestMagic(Assembler::Equal, slot, failure->label());

    // Load the value.
    masm.loadValue(slot, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadEnvironmentDynamicSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);

    // Check for uninitialized lexicals.
    BaseIndex slot(scratch2, scratch, TimesOne);
    masm.branchTestMagic(Assembler::Equal, slot, failure->label());

    // Load the value.
    masm.loadValue(slot, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::callTypeUpdateIC(AutoStubFrame& stubFrame, Register obj, ValueOperand val,
                                          Register scratch, LiveGeneralRegisterSet saveRegs)
{
    // R0 contains the value that needs to be typechecked.
    MOZ_ASSERT(val == R0);
    MOZ_ASSERT(scratch == R1.scratchReg());

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    static const bool CallClobbersTailReg = false;
#else
    static const bool CallClobbersTailReg = true;
#endif

    // Call the first type update stub.
    if (CallClobbersTailReg)
        masm.push(ICTailCallReg);
    masm.push(ICStubReg);
    masm.loadPtr(Address(ICStubReg, ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);
    masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));
    masm.pop(ICStubReg);
    if (CallClobbersTailReg)
        masm.pop(ICTailCallReg);

    // The update IC will store 0 or 1 in |scratch|, R1.scratchReg(), reflecting
    // if the value in R0 type-checked properly or not.
    Label done;
    masm.branch32(Assembler::Equal, scratch, Imm32(1), &done);

    stubFrame.enter(masm, scratch, CallCanGC::CanNotGC);

    masm.PushRegsInMask(saveRegs);

    masm.Push(val);
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
    masm.Push(ICStubReg);

    // Load previous frame pointer, push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), scratch);
    masm.pushBaselineFramePtr(scratch, scratch);

    if (!callVM(masm, DoTypeUpdateFallbackInfo))
        return false;

    masm.PopRegsInMask(saveRegs);

    stubFrame.leave(masm);

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreSlotShared(bool isFixed)
{
    ObjOperandId objId = reader.objOperandId();
    Address offsetAddr = stubAddress(reader.stubOffset());

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoStubFrame stubFrame(*this);
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);

    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(val);
    if (!callTypeUpdateIC(stubFrame, obj, val, scratch, saveRegs))
        return false;

    masm.load32(offsetAddr, scratch);

    if (isFixed) {
        BaseIndex slot(obj, scratch, TimesOne);
        EmitPreBarrier(masm, slot, MIRType::Value);
        masm.storeValue(val, slot);
    } else {
        // To avoid running out of registers on x86, use ICStubReg as scratch.
        // We don't need it anymore.
        Register slots = ICStubReg;
        masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), slots);
        BaseIndex slot(slots, scratch, TimesOne);
        EmitPreBarrier(masm, slot, MIRType::Value);
        masm.storeValue(val, slot);
    }

    if (cx_->gc.nursery.exists())
        BaselineEmitPostWriteBarrierSlot(masm, obj, val, scratch, LiveGeneralRegisterSet(), cx_);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreFixedSlot()
{
    return emitStoreSlotShared(true);
}

bool
BaselineCacheIRCompiler::emitStoreDynamicSlot()
{
    return emitStoreSlotShared(false);
}

bool
BaselineCacheIRCompiler::emitStoreUnboxedProperty()
{
    ObjOperandId objId = reader.objOperandId();
    JSValueType fieldType = reader.valueType();
    Address offsetAddr = stubAddress(reader.stubOffset());

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoStubFrame stubFrame(*this);
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);

    // We only need the type update IC if we are storing an object.
    if (fieldType == JSVAL_TYPE_OBJECT) {
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(obj);
        saveRegs.add(val);
        if (!callTypeUpdateIC(stubFrame, obj, val, scratch, saveRegs))
            return false;
    }

    masm.load32(offsetAddr, scratch);
    BaseIndex fieldAddr(obj, scratch, TimesOne);

    // Note that the storeUnboxedProperty call here is infallible, as the
    // IR emitter is responsible for guarding on |val|'s type.
    EmitUnboxedPreBarrierForBaseline(masm, fieldAddr, fieldType);
    masm.storeUnboxedProperty(fieldAddr, fieldType,
                              ConstantOrRegister(TypedOrValueRegister(val)),
                              /* failure = */ nullptr);

    if (UnboxedTypeNeedsPostBarrier(fieldType))
        BaselineEmitPostWriteBarrierSlot(masm, obj, val, scratch, LiveGeneralRegisterSet(), cx_);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreTypedObjectReferenceProperty()
{
    ObjOperandId objId = reader.objOperandId();
    Address offsetAddr = stubAddress(reader.stubOffset());
    TypedThingLayout layout = reader.typedThingLayout();
    ReferenceTypeDescr::Type type = reader.referenceTypeDescrType();

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoStubFrame stubFrame(*this);
    AutoScratchRegister scratch1(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);

    // We don't need a type update IC if the property is always a string.
    if (type != ReferenceTypeDescr::TYPE_STRING) {
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(obj);
        saveRegs.add(val);
        if (!callTypeUpdateIC(stubFrame, obj, val, scratch1, saveRegs))
            return false;
    }

    // Compute the address being written to.
    LoadTypedThingData(masm, layout, obj, scratch1);
    masm.addPtr(offsetAddr, scratch1);
    Address dest(scratch1, 0);

    // To avoid running out of registers on x86, use ICStubReg as second
    // scratch. It won't be used after this.
    Register scratch2 = ICStubReg;

    switch (type) {
      case ReferenceTypeDescr::TYPE_ANY:
        EmitPreBarrier(masm, dest, MIRType::Value);
        masm.storeValue(val, dest);
        break;

      case ReferenceTypeDescr::TYPE_OBJECT: {
        EmitPreBarrier(masm, dest, MIRType::Object);
        Label isNull, done;
        masm.branchTestObject(Assembler::NotEqual, val, &isNull);
        masm.unboxObject(val, scratch2);
        masm.storePtr(scratch2, dest);
        masm.jump(&done);
        masm.bind(&isNull);
        masm.storePtr(ImmWord(0), dest);
        masm.bind(&done);
        break;
      }

      case ReferenceTypeDescr::TYPE_STRING:
        EmitPreBarrier(masm, dest, MIRType::String);
        masm.unboxString(val, scratch2);
        masm.storePtr(scratch2, dest);
        break;
    }

    if (type != ReferenceTypeDescr::TYPE_STRING)
        BaselineEmitPostWriteBarrierSlot(masm, obj, val, scratch1, LiveGeneralRegisterSet(), cx_);

    return true;
}

bool
BaselineCacheIRCompiler::emitStoreTypedObjectScalarProperty()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address offsetAddr = stubAddress(reader.stubOffset());
    TypedThingLayout layout = reader.typedThingLayout();
    Scalar::Type type = reader.scalarType();
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Compute the address being written to.
    LoadTypedThingData(masm, layout, obj, scratch1);
    masm.addPtr(offsetAddr, scratch1);
    Address dest(scratch1, 0);

    BaselineStoreToTypedArray(cx_, masm, type, val, dest, scratch2,
                              failure->label(), failure->label());

    return true;
}

bool
BaselineCacheIRCompiler::emitTypeMonitorResult()
{
    allocator.discardStack(masm);
    EmitEnterTypeMonitorIC(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitReturnFromIC()
{
    allocator.discardStack(masm);
    EmitReturnFromIC(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadObject()
{
    Register reg = allocator.defineRegister(masm, reader.objOperandId());
    masm.loadPtr(stubAddress(reader.stubOffset()), reg);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardDOMExpandoMissingOrGuardShape()
{
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    AutoScratchRegister shapeScratch(allocator, masm);
    AutoScratchRegister objScratch(allocator, masm);
    Address shapeAddr(stubAddress(reader.stubOffset()));

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label done;
    masm.branchTestUndefined(Assembler::Equal, val, &done);

    masm.debugAssertIsObject(val);
    masm.loadPtr(shapeAddr, shapeScratch);
    masm.unboxObject(val, objScratch);
    masm.branchTestObjShape(Assembler::NotEqual, objScratch, shapeScratch, failure->label());

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadDOMExpandoValueGuardGeneration()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address expandoAndGenerationAddr(stubAddress(reader.stubOffset()));
    Address generationAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);
    ValueOperand output = allocator.defineValueRegister(masm, reader.valOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.loadPtr(Address(obj, ProxyObject::offsetOfValues()), scratch);
    Address expandoAddr(scratch, ProxyObject::offsetOfExtraSlotInValues(GetDOMProxyExpandoSlot()));

    // Load the ExpandoAndGeneration* in the output scratch register and guard
    // it matches the proxy's ExpandoAndGeneration.
    masm.loadPtr(expandoAndGenerationAddr, output.scratchReg());
    masm.branchPrivatePtr(Assembler::NotEqual, expandoAddr, output.scratchReg(), failure->label());

    // Guard expandoAndGeneration->generation matches the expected generation.
    masm.branch64(Assembler::NotEqual,
                  Address(output.scratchReg(), ExpandoAndGeneration::offsetOfGeneration()),
                  generationAddr,
                  scratch, failure->label());

    // Load expandoAndGeneration->expando into the output Value register.
    masm.loadValue(Address(output.scratchReg(), ExpandoAndGeneration::offsetOfExpando()), output);
    return true;
}

bool
BaselineCacheIRCompiler::init(CacheKind kind)
{
    if (!allocator.init())
        return false;

    // Baseline ICs monitor values when needed, so returning doubles is fine.
    allowDoubleResult_.emplace(true);

    size_t numInputs = writer_.numInputOperands();
    AllocatableGeneralRegisterSet available(ICStubCompiler::availableGeneralRegs(numInputs));

    switch (kind) {
      case CacheKind::GetProp:
        MOZ_ASSERT(numInputs == 1);
        allocator.initInputLocation(0, R0);
        break;
      case CacheKind::GetElem:
      case CacheKind::SetProp:
        MOZ_ASSERT(numInputs == 2);
        allocator.initInputLocation(0, R0);
        allocator.initInputLocation(1, R1);
        break;
      case CacheKind::GetName:
        MOZ_ASSERT(numInputs == 1);
        allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_OBJECT);
#if defined(JS_NUNBOX32)
        // availableGeneralRegs can't know that GetName is only using
        // the payloadReg and not typeReg on x86.
        available.add(R0.typeReg());
#endif
        break;
    }

    allocator.initAvailableRegs(available);
    outputUnchecked_.emplace(R0);
    return true;
}

static const size_t MaxOptimizedCacheIRStubs = 16;

ICStub*
jit::AttachBaselineCacheIRStub(JSContext* cx, const CacheIRWriter& writer,
                               CacheKind kind, ICStubEngine engine, JSScript* outerScript,
                               ICFallbackStub* stub)
{
    // We shouldn't GC or report OOM (or any other exception) here.
    AutoAssertNoPendingException aanpe(cx);
    JS::AutoCheckCannotGC nogc;

    if (writer.failed())
        return nullptr;

    // Just a sanity check: the caller should ensure we don't attach an
    // unlimited number of stubs.
    MOZ_ASSERT(stub->numOptimizedStubs() < MaxOptimizedCacheIRStubs);

    enum class CacheIRStubKind { Monitored, Updated };

    uint32_t stubDataOffset;
    CacheIRStubKind stubKind;
    switch (kind) {
      case CacheKind::GetProp:
      case CacheKind::GetElem:
      case CacheKind::GetName:
        stubDataOffset = sizeof(ICCacheIR_Monitored);
        stubKind = CacheIRStubKind::Monitored;
        break;
      case CacheKind::SetProp:
        stubDataOffset = sizeof(ICCacheIR_Updated);
        stubKind = CacheIRStubKind::Updated;
        break;
    }

    JitCompartment* jitCompartment = cx->compartment()->jitCompartment();

    // Check if we already have JitCode for this stub.
    CacheIRStubInfo* stubInfo;
    CacheIRStubKey::Lookup lookup(kind, engine, writer.codeStart(), writer.codeLength());
    JitCode* code = jitCompartment->getCacheIRStubCode(lookup, &stubInfo);
    if (!code) {
        // We have to generate stub code.
        JitContext jctx(cx, nullptr);
        BaselineCacheIRCompiler comp(cx, writer, engine, stubDataOffset);
        if (!comp.init(kind))
            return nullptr;

        code = comp.compile();
        if (!code)
            return nullptr;

        // Allocate the shared CacheIRStubInfo. Note that the putCacheIRStubCode
        // call below will transfer ownership to the stub code HashMap, so we
        // don't have to worry about freeing it below.
        MOZ_ASSERT(!stubInfo);
        stubInfo = CacheIRStubInfo::New(kind, engine, comp.makesGCCalls(), stubDataOffset, writer);
        if (!stubInfo)
            return nullptr;

        CacheIRStubKey key(stubInfo);
        if (!jitCompartment->putCacheIRStubCode(lookup, key, code))
            return nullptr;
    }

    MOZ_ASSERT(code);
    MOZ_ASSERT(stubInfo);
    MOZ_ASSERT(stubInfo->stubDataSize() == writer.stubDataSize());

    // Ensure we don't attach duplicate stubs. This can happen if a stub failed
    // for some reason and the IR generator doesn't check for exactly the same
    // conditions.
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        switch (stubKind) {
          case CacheIRStubKind::Monitored: {
            if (!iter->isCacheIR_Monitored())
                continue;
            auto otherStub = iter->toCacheIR_Monitored();
            if (otherStub->stubInfo() != stubInfo)
                continue;
            if (!writer.stubDataEqualsMaybeUpdate(otherStub->stubDataStart()))
                continue;
            break;
          }
          case CacheIRStubKind::Updated: {
            if (!iter->isCacheIR_Updated())
                continue;
            auto otherStub = iter->toCacheIR_Updated();
            if (otherStub->stubInfo() != stubInfo)
                continue;
            if (!writer.stubDataEqualsMaybeUpdate(otherStub->stubDataStart()))
                continue;
            break;
          }
        }

        // We found a stub that's exactly the same as the stub we're about to
        // attach. Just return nullptr, the caller should do nothing in this
        // case.
        return nullptr;
    }

    // Time to allocate and attach a new stub.

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();

    ICStubSpace* stubSpace = ICStubCompiler::StubSpaceForStub(stubInfo->makesGCCalls(),
                                                              outerScript, engine);
    void* newStubMem = stubSpace->alloc(bytesNeeded);
    if (!newStubMem)
        return nullptr;

    switch (stubKind) {
      case CacheIRStubKind::Monitored: {
        ICStub* monitorStub =
            stub->toMonitoredFallbackStub()->fallbackMonitorStub()->firstMonitorStub();
        auto newStub = new(newStubMem) ICCacheIR_Monitored(code, monitorStub, stubInfo);
        writer.copyStubData(newStub->stubDataStart());
        stub->addNewStub(newStub);
        return newStub;
      }
      case CacheIRStubKind::Updated: {
        auto newStub = new(newStubMem) ICCacheIR_Updated(code, stubInfo);
        if (!newStub->initUpdatingChain(cx, stubSpace)) {
            cx->recoverFromOutOfMemory();
            return nullptr;
        }
        writer.copyStubData(newStub->stubDataStart());
        stub->addNewStub(newStub);
        return newStub;
      }
    }

    MOZ_CRASH("Invalid kind");
}

uint8_t*
ICCacheIR_Monitored::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

uint8_t*
ICCacheIR_Updated::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

/* static */ ICCacheIR_Monitored*
ICCacheIR_Monitored::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                           ICCacheIR_Monitored& other)
{
    const CacheIRStubInfo* stubInfo = other.stubInfo();
    MOZ_ASSERT(stubInfo->makesGCCalls());

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();
    void* newStub = space->alloc(bytesNeeded);
    if (!newStub)
        return nullptr;

    ICCacheIR_Monitored* res = new(newStub) ICCacheIR_Monitored(other.jitCode(), firstMonitorStub,
                                                                stubInfo);
    stubInfo->copyStubData(&other, res);
    return res;
}
