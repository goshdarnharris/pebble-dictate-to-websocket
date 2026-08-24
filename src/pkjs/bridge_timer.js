'use strict';

function BridgeTimer(durationMs, timerApi) {
  this.durationMs = durationMs;
  this.timerApi = timerApi;
  this.handle = null;
}

BridgeTimer.prototype.start = function(callback) {
  this.cancel();
  this.handle = this.timerApi.setTimeout(function() {
    this.handle = null;
    callback();
  }.bind(this), this.durationMs);
};

BridgeTimer.prototype.cancel = function() {
  if (this.handle !== null) {
    this.timerApi.clearTimeout(this.handle);
    this.handle = null;
  }
};

BridgeTimer.prototype.isActive = function() {
  return this.handle !== null;
};

module.exports = BridgeTimer;