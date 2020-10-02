/*
 * Copyright (c) 2015, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
// ...Except third-party code below.


// Avoid `console` errors in browsers that lack a console.
(function() {
    var method;
    var noop = function () {};
    var methods = [
        'assert', 'clear', 'count', 'debug', 'dir', 'dirxml', 'error',
        'exception', 'group', 'groupCollapsed', 'groupEnd', 'info', 'log',
        'markTimeline', 'profile', 'profileEnd', 'table', 'time', 'timeEnd',
        'timeline', 'timelineEnd', 'timeStamp', 'trace', 'warn'
    ];
    var length = methods.length;
    var console = (window.console = window.console || {});

    while (length--) {
        method = methods[length];

        // Only stub undefined methods.
        if (!console[method]) {
            console[method] = noop;
        }
    }
}());

// Place any jQuery/helper plugins in here.


/*!
 * jQuery.scrollTo
 * Copyright (c) 2007 Ariel Flesler - aflesler ○ gmail • com | https://github.com/flesler
 * Licensed under MIT
 * https://github.com/flesler/jquery.scrollTo
 * @projectDescription Lightweight, cross-browser and highly customizable animated scrolling with jQuery
 * @author Ariel Flesler
 * @version 2.1.2
 */
;
(function (factory) {
  'use strict';
  if (typeof define === 'function' && define.amd) {
    // AMD
    define(['jquery'], factory);
  } else if (typeof module !== 'undefined' && module.exports) {
    // CommonJS
    module.exports = factory(require('jquery'));
  } else {
    // Global
    factory(jQuery);
  }
})(function ($) {
  'use strict';

  var $scrollTo = $.scrollTo = function (target, duration, settings) {
    return $(window).scrollTo(target, duration, settings);
  };

  $scrollTo.defaults = {
    axis: 'xy',
    duration: 0,
    limit: true
  };

  function isWin(elem) {
    return !elem.nodeName ||
      $.inArray(elem.nodeName.toLowerCase(), ['iframe', '#document', 'html', 'body']) !== -1;
  }

  $.fn.scrollTo = function (target, duration, settings) {
    if (typeof duration === 'object') {
      settings = duration;
      duration = 0;
    }
    if (typeof settings === 'function') {
      settings = {
        onAfter: settings
      };
    }
    if (target === 'max') {
      target = 9e9;
    }

    settings = $.extend({}, $scrollTo.defaults, settings);
    // Speed is still recognized for backwards compatibility
    duration = duration || settings.duration;
    // Make sure the settings are given right
    var queue = settings.queue && settings.axis.length > 1;
    if (queue) {
      // Let's keep the overall duration
      duration /= 2;
    }
    settings.offset = both(settings.offset);
    settings.over = both(settings.over);

    return this.each(function () {
      // Null target yields nothing, just like jQuery does
      if (target === null) return;

      var win = isWin(this),
        elem = win ? this.contentWindow || window : this,
        $elem = $(elem),
        targ = target,
        attr = {},
        toff;

      switch (typeof targ) {
        // A number will pass the regex
        case 'number':
        case 'string':
          if (/^([+-]=?)?\d+(\.\d+)?(px|%)?$/.test(targ)) {
            targ = both(targ);
            // We are done
            break;
          }
          // Relative/Absolute selector
          targ = win ? $(targ) : $(targ, elem);
          /* falls through */
        case 'object':
          if (targ.length === 0) return;
          // DOMElement / jQuery
          if (targ.is || targ.style) {
            // Get the real position of the target
            toff = (targ = $(targ)).offset();
          }
      }

      var offset = $.isFunction(settings.offset) && settings.offset(elem, targ) || settings.offset;

      $.each(settings.axis.split(''), function (i, axis) {
        var Pos = axis === 'x' ? 'Left' : 'Top',
          pos = Pos.toLowerCase(),
          key = 'scroll' + Pos,
          prev = $elem[key](),
          max = $scrollTo.max(elem, axis);

        if (toff) { // jQuery / DOMElement
          attr[key] = toff[pos] + (win ? 0 : prev - $elem.offset()[pos]);

          // If it's a dom element, reduce the margin
          if (settings.margin) {
            attr[key] -= parseInt(targ.css('margin' + Pos), 10) || 0;
            attr[key] -= parseInt(targ.css('border' + Pos + 'Width'), 10) || 0;
          }

          attr[key] += offset[pos] || 0;

          if (settings.over[pos]) {
            // Scroll to a fraction of its width/height
            attr[key] += targ[axis === 'x' ? 'width' : 'height']() * settings.over[pos];
          }
        } else {
          var val = targ[pos];
          // Handle percentage values
          attr[key] = val.slice && val.slice(-1) === '%' ?
            parseFloat(val) / 100 * max :
            val;
        }

        // Number or 'number'
        if (settings.limit && /^\d+$/.test(attr[key])) {
          // Check the limits
          attr[key] = attr[key] <= 0 ? 0 : Math.min(attr[key], max);
        }

        // Don't waste time animating, if there's no need.
        if (!i && settings.axis.length > 1) {
          if (prev === attr[key]) {
            // No animation needed
            attr = {};
          } else if (queue) {
            // Intermediate animation
            animate(settings.onAfterFirst);
            // Don't animate this axis again in the next iteration.
            attr = {};
          }
        }
      });

      animate(settings.onAfter);

      function animate(callback) {
        var opts = $.extend({}, settings, {
          // The queue setting conflicts with animate()
          // Force it to always be true
          queue: true,
          duration: duration,
          complete: callback && function () {
            callback.call(elem, targ, settings);
          }
        });
        $elem.animate(attr, opts);
      }
    });
  };

  // Max scrolling position, works on quirks mode
  // It only fails (not too badly) on IE, quirks mode.
  $scrollTo.max = function (elem, axis) {
    var Dim = axis === 'x' ? 'Width' : 'Height',
      scroll = 'scroll' + Dim;

    if (!isWin(elem))
      return elem[scroll] - $(elem)[Dim.toLowerCase()]();

    var size = 'client' + Dim,
      doc = elem.ownerDocument || elem.document,
      html = doc.documentElement,
      body = doc.body;

    return Math.max(html[scroll], body[scroll]) - Math.min(html[size], body[size]);
  };

  function both(val) {
    return $.isFunction(val) || $.isPlainObject(val) ? val : {
      top: val,
      left: val
    };
  }

  // Add special hooks so that window scroll properties can be animated
  $.Tween.propHooks.scrollLeft =
    $.Tween.propHooks.scrollTop = {
      get: function (t) {
        return $(t.elem)[t.prop]();
      },
      set: function (t) {
        var curr = this.get(t);
        // If interrupt is true and user scrolled, stop animating
        if (t.options.interrupt && t._last && t._last !== curr) {
          return $(t.elem).stop();
        }
        var next = Math.round(t.now);
        // Don't waste CPU
        // Browsers don't render floating point scroll
        if (curr !== next) {
          $(t.elem)[t.prop](next);
          t._last = this.get(t);
        }
      }
    };

  // AMD requirement
  return $scrollTo;
});

// Smarter resize event
// http://www.paulirish.com/2009/throttled-smartresize-jquery-event-handler/
(function($,sr){
  // debouncing function from John Hann
  // http://unscriptable.com/index.php/2009/03/20/debouncing-javascript-methods/
  var debounce = function (func, threshold, execAsap) {
      var timeout;

      return function debounced () {
          var obj = this, args = arguments;
          function delayed () {
              if (!execAsap)
                  func.apply(obj, args);
              timeout = null;
          }

          if (timeout)
              clearTimeout(timeout);
          else if (execAsap)
              func.apply(obj, args);

          timeout = setTimeout(delayed, threshold || 100);
      };
  };
  // smartresize
  jQuery.fn[sr] = function(fn){  return fn ? this.bind('resize', debounce(fn)) : this.trigger(sr); };

})(jQuery,'smartresize');


/*
Datastore example:

const store = new Datastore({
   obj: {
     nestedObj: {
       a: 'a',
       b: 'b'
     }
   },
   integer: 123,
   array: [1, 2, 3]
});

const unsub = store.subscribe('obj.nestedObj.a', (path, data, type) => {
   console.log('deeper', path, data, type);
   // unsubscribe
   unsub();
});

store.subscribe('obj.nestedObj', (path, data, type) => {
   console.log('shallower', path, data, type);
});

store.set('obj.nestedObj.c', 'c'); // add a new property
// >> shallower obj.nestedObj {a: "a", b: "b", c: "c"} change

store.set('obj.nestedObj.a', 'aa'); // will trigger both subscribers
// >> deeper obj.nestedObj.a aa change
// >> shallower obj.nestedObj {a: "aa", b: "b", c: "c"} change

// Does nothing, as the value is unchanged.
store.set('obj.nestedObj.a', 'aa');

// Value is still unchanged, but we're specifying a `false` equality operator, so the
// subscriptions will fire.
store.set('obj.nestedObj.a', 'aa', false);
// >> shallower obj.nestedObj {a: "aa", b: "b", c: "c"} change
// The deeper subscription already unsubscribed itself.

// The underlying data can be accessed directly. But don't modify it, unless you're trying
// to bypass the immutability and pubsub!
console.dir(store.data);

This example has been left out of @example tags so as to not flood the pop-up help.
*/

/**
 * A state and data store featuring immutability of the underlying object and ability to
 * subscribe to changes.
 * For immutability this library is required: https://github.com/mariocasciaro/object-path-immutable
 * For object-path subscribing and access, lodash is required: https://lodash.com/docs/3.10.1#get
 * @param {object} data The initial data structure. For ease of code understandability,
 *                      this should be approximately the structure the data will always have.
 * @param {?string} name The name of the store; used only for help logging and debugging.
 */
function Datastore(data, name) {
  /**
   * The name of the datastore
   * @type {string}
   * @readonly
   */
  this.name = name || "Datastore-".concat(Math.random());

  /**
   * The underlying data object. This can be read directly to access the data, but should
   * NOT be modified (unless you're specifically trying to avoid immutability and
   * subscriber notifications).
   * @type {object}
   */
  this.data = data;

  /**
   * [{path: "a.b.c", func: callback, key: unique},...]
   */
  this._subscribers = [];

  /**
   * The possible event types.
   * TODO: Are types even necessary? Right now we only use 'change'.
   * @enum {string}
   * @readonly
   */
  this.EventTypes = {
    change: 'change'
  };

  /**
   * Send the specified change to the subscribers.
   * @param {string} path The object-path of the change.
   * @param {EventTypes} type The type of the change.
   */
  this._dispatch = function(path, type) {
    // this.data might change (immutably) during the timeout, so capture the current
    // object now.
    const data = this.data;

    // Ensure this is asynchronous
    setTimeout(()=> {
      // When looking for subscribers, we look at prefixes of paths, as whole sub-trees
      // can be subscribed to.
      const subs = _.filter(this._subscribers, (s) => {
        // We append '.' because we want "a.b" to match "a.b.c" but not "a.banana".
        return _.startsWith(path+'.', s.path+'.');
      });

      for (let i = 0; i < subs.length; i++) {
        let sub = subs[i];
        // The amount of data (tree or specific value) that a subscriber gets depends on
        // what they are subscribed to.
        sub.func(sub.path, _.get(data, sub.path), type);
      }
    }, 0);
  };

  /**
   * Returns data wrapped with https://github.com/mariocasciaro/object-path-immutable
   * @param {object} data If falsy, this.data will be wrapped.
   * @returns {objectPathImmutable}
   */
  this._imm = function(data) {
    if (!_.isUndefined(data)) {
      return objectPathImmutable(data);
    }
    return objectPathImmutable(this.data);
  };

  /**
   * Check if val1 and val2 are equal, using equality or _.isEqual if not supplied.
   * @param {any} val1
   * @param {any} val2
   * @param {?(function|boolean)} equality The equality comparison function. If not
   *    supplied, _.isEqual (deep equality) will be used. If `false`, no equality comparison
   *    will be done. If supplied, it must take val1 and val2 and return a boolean.
   * @returns {boolean}
   */
  this._eq = function(val1, val2, equality) {
    if (equality === false) {
      // Caller doesn't want the equality check
      return false;
    }
    if (equality) {
      return equality(val1, val2);
    }
    return _.isEqual(val1, val2);
  };
}

Datastore.prototype = {
  /**
   * Add or modify a value in the data.
   * For path details see: https://github.com/mariocasciaro/object-path-immutable
   * However, the string form _must_ be used.
   * @param {string} path The object-path to set. Like 'a' or 'a.b'.
   * @param {any} value The value to set.
   * @param {?(function|boolean)} equality The equality comparison function. If not
   *    supplied, _.isEqual (deep equality) will be used. If `false`, no equality comparison
   *    will be done. If supplied, it must take old and new values and return a boolean.
   */
  set: function(path, value, equality) {
    const newData = this._imm().set(path, value).value();
    const oldVal = _.get(this.data, path);
    const newVal = _.get(newData, path);

    if (this._eq(oldVal, newVal, equality)) {
      // No change; don't update or dispatch
      return;
    }

    // Update our data
    this.data = newData;
    // Tell the subscribers
    this._dispatch(path, this.EventTypes.change);
  },

  // TODO: Add needed API shims for https://github.com/mariocasciaro/object-path-immutable

  /**
   * Subscribe to changes in the data store, for changes at or below path; when changes
   * occur func will be called.
   * @param {string} path The object-path that will be subscribed to. This can be a leaf
   *    value or branch higher up. The dotted-string form of the path must be used.
   *    If falsy or '', the subscription will be to the whole data object.
   * @param {function} func The function that will be called whenever a value at or under
   *    the path is changed. It will be called like `func(path, data, type)`, where `path`
   *    is the same path passed in here, `data` is the data at that path (possibly a
   *    subtree if subscribe to a non-leaf), and `type` will be one of `EventTypes`,
   *    depending on the change type.
   * @returns {function} The unsubscribe function.
   */
  subscribe: function(path, func) {
    // Empty string works as subscribe-to-all because _.startsWith will always return true for it.
    path = path || '';

    const key = String(Math.random());
    const unsubscribe = () => {
      _.remove(this._subscribers, (s) => { return s.key === key; });
    };
    this._subscribers.push({func, path, key});
    return unsubscribe;
  }
};
