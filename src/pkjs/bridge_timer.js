'use strict';

function BridgeTimer(durationMs, timerApi) {
  this.durationMs = durationMs;
  this.timerApi = timerApi;
  this.handle = null;
}

BridgeTimer.prototype.start = function(callback) {
  this.cancel();
  var setTimer = this.timerApi.setTimeout;
  this.handle = setTimer(function() {
    this.handle = null;
    callback();
  }.bind(this), this.durationMs);
};

BridgeTimer.prototype.cancel = function() {
  if (this.handle !== null) {
    var clearTimer = this.timerApi.clearTimeout;
    clearTimer(this.handle);
    this.handle = null;
  }
};

BridgeTimer.prototype.isActive = function() {
  return this.handle !== null;
};

module.exports = BridgeTimer;