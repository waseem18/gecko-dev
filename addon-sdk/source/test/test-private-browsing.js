/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
'use strict';

const { Ci, Cu } = require('chrome');
const { safeMerge } = require('sdk/util/object');
const windows = require('sdk/windows').browserWindows;
const tabs = require('sdk/tabs');
const winUtils = require('sdk/window/utils');
const { isWindowPrivate } = winUtils;
const { isPrivateBrowsingSupported } = require('sdk/self');
const { is } = require('sdk/system/xul-app');
const { isPrivate } = require('sdk/private-browsing');
const { LoaderWithHookedConsole } = require("sdk/test/loader");
const { getMode, isGlobalPBSupported,
        isWindowPBSupported, isTabPBSupported } = require('sdk/private-browsing/utils');
const { pb } = require('./private-browsing/helper');
const prefs = require('sdk/preferences/service');
const { set: setPref } = require("sdk/preferences/service");
const DEPRECATE_PREF = "devtools.errorconsole.deprecation_warnings";

const { Services } = Cu.import("resource://gre/modules/Services.jsm", {});

const kAutoStartPref = "browser.privatebrowsing.autostart";

// is global pb is enabled?
if (isGlobalPBSupported) {
  safeMerge(module.exports, require('./private-browsing/global'));

  exports.testGlobalOnlyOnFirefox = function(assert) {
    assert.ok(is("Firefox"), "isGlobalPBSupported is only true on Firefox");
  }
}
else if (isWindowPBSupported) {
  safeMerge(module.exports, require('./private-browsing/windows'));

  exports.testPWOnlyOnFirefox = function(assert) {
    assert.ok(is("Firefox"), "isWindowPBSupported is only true on Firefox");
  }
}
// only on Fennec
else if (isTabPBSupported) {
  safeMerge(module.exports, require('./private-browsing/tabs'));

  exports.testPTOnlyOnFennec = function(assert) {
    assert.ok(is("Fennec"), "isTabPBSupported is only true on Fennec");
  }
}

exports.testIsPrivateDefaults = function(assert) {
  assert.equal(isPrivate(), false, 'undefined is not private');
  assert.equal(isPrivate('test'), false, 'strings are not private');
  assert.equal(isPrivate({}), false, 'random objects are not private');
  assert.equal(isPrivate(4), false, 'numbers are not private');
  assert.equal(isPrivate(/abc/), false, 'regex are not private');
  assert.equal(isPrivate(function() {}), false, 'functions are not private');
};

exports.testWindowDefaults = function(assert) {
  setPref(DEPRECATE_PREF, true);
  // Ensure that browserWindow still works while being deprecated
  let { loader, messages } = LoaderWithHookedConsole(module);
  let windows = loader.require("sdk/windows").browserWindows;
  assert.equal(windows.activeWindow.isPrivateBrowsing, false,
                   'window is not private browsing by default');
  assert.ok(/DEPRECATED.+isPrivateBrowsing/.test(messages[0].msg),
                     'isPrivateBrowsing is deprecated');

  let chromeWin = winUtils.getMostRecentBrowserWindow();
  assert.equal(getMode(chromeWin), false);
  assert.equal(isWindowPrivate(chromeWin), false);
};

// tests for the case where private browsing doesn't exist
exports.testIsActiveDefault = function(assert) {
  assert.equal(pb.isActive, false,
                   'pb.isActive returns false when private browsing isn\'t supported');
};

exports.testIsPrivateBrowsingFalseDefault = function(assert) {
  assert.equal(isPrivateBrowsingSupported, false,
  	               'isPrivateBrowsingSupported property is false by default');
};

exports.testNSIPrivateBrowsingChannel = function(assert) {
  let channel = Services.io.newChannel("about:blank", null, null);
  channel.QueryInterface(Ci.nsIPrivateBrowsingChannel);
  assert.equal(isPrivate(channel), false, 'isPrivate detects non-private channels');
  channel.setPrivate(true);
  assert.ok(isPrivate(channel), 'isPrivate detects private channels');
}

exports.testNewGlobalPBService = function(assert) {
  assert.equal(isPrivate(), false, 'isPrivate() is false by default');
  prefs.set(kAutoStartPref, true);
  assert.equal(prefs.get(kAutoStartPref, false), true, kAutoStartPref + ' is true now');
  assert.equal(isPrivate(), true, 'isPrivate() is true now');
  prefs.set(kAutoStartPref, false);
  assert.equal(isPrivate(), false, 'isPrivate() is false again');
};

require('sdk/test').run(exports);
