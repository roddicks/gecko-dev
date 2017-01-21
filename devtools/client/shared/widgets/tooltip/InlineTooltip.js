/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EventEmitter = require("devtools/shared/event-emitter");

function InlineTooltip(doc, {
    closeOnEvents = []
  } = {}) {
  EventEmitter.decorate(this);

  this.doc = doc;
  this.closeOnEvents = closeOnEvents;

  this.panel = this.doc.createElement("div");
  this.panelHTML = this.panel.innerHTML;
  this.clear();

  console.log("CREATED");

  // // Listen to custom emitters' events to close the tooltip
  // this.hide = this.hide.bind(this);
  // let closeOnEvents = this.options.get("closeOnEvents");
  // for (let {emitter, event, useCapture} of closeOnEvents) {
  //   for (let add of ["addEventListener", "on"]) {
  //     if (add in emitter) {
  //       emitter[add](event, this.hide, useCapture);
  //       break;
  //     }
  //   }
  // }
}

InlineTooltip.prototype = {
  /**
   * Show the tooltip. It might be wise to append some content first if you
   * don't want the tooltip to be empty. You may access the content of the
   * tooltip by setting a XUL node to t.content.
   *
   * @param {DOMNode} anchor
   *        Which node should the tooltip be shown on
   */
  show(anchor) {
    if (this.isVisible()) {
      return;
    }

    this.panel.innerHTML = this.panelHTML;

    let propertyContainer = anchor.closest(".ruleview-propertycontainer");

    // Insert the inline editor next to the property
    if (!this.panel.parentNode) {
      propertyContainer.parentNode.insertBefore(this.panel,
                                                propertyContainer.nextSibling);
    }

    this.emit("shown");
  },

  /**
   * Hide the current tooltip.
   */
  hide() {
    if (!this.isVisible()) {
      return;
    }

    this.panelHTML = this.panel.innerHTML;
    this.clear();

    this.emit("hidden");
  },

  /**
   * Check if the tooltip is currently displayed.
   *
   * @return {Boolean} true if the tooltip is not empty
   */
  isVisible() {
    return this.panel.innerHTML;
  },

  clear() {
    this.panel.innerHTML = "";
  },

  /**
   * Set the content of this tooltip. Will first clear the tooltip and then
   * append the new content element.
   * Consider using one of the set<type>Content() functions instead.
   *
   * @param {DOMNode} content
   *        A node that can be appended in the tooltip XUL element
   */
  setContent(content) {
    if (this.content == content) {
      return;
    }

    this.clear();

    if (content) {
      this.panel.appendChild(content);
    }
  },

  get content() {
    return this.panel.firstChild;
  },

  destroy() {

    // let closeOnEvents = this.options.get("closeOnEvents");
    // for (let {emitter, event, useCapture} of closeOnEvents) {
    //   for (let remove of ["removeEventListener", "off"]) {
    //     if (remove in emitter) {
    //       emitter[remove](event, this.hide, useCapture);
    //       break;
    //     }
    //   }
    // }

    this.panel.parentNode.removeChild(this.panel);
    this.doc = null;
    this.panel = null;
    this.closeOnEvents = null;
  },
};

module.exports = InlineTooltip;
