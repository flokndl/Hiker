var isTracking = false;
var startLat = null;
var startLon = null;
var lastLat = null;
var lastLon = null;
var totalDistanceMeters = 0;
var watchPositionId = null;

var totalAscent = 0;
var totalDescent = 0;
var lastAlt = null;
var pointsCollectedInSession = 0;

// NEW FIX: Added a switch to prevent non-essential debugs from cluttering the watch
function sendDebug(msg, showOnWatch) {
  console.log("DEBUG: " + msg);
  var dict = {
    'KEY_DEBUG_MSG': showOnWatch ? msg.substring(0, 60) : ""
  };
  Pebble.sendAppMessage(dict);
}

function calculateDistance(lat1, lon1, lat2, lon2) {
  var R = 6371000; 
  var dLat = (lat2 - lat1) * Math.PI / 180;
  var dLon = (lon2 - lon1) * Math.PI / 180;
  var a = Math.sin(dLat/2) * Math.sin(dLat/2) +
          Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
          Math.sin(dLon/2) * Math.sin(dLon/2);
  var c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
  return R * c;
}

function getMeterOffsets(lat, lon) {
  var yOffset = (lat - startLat) * 111111;
  var xOffset = (lon - startLon) * 111111 * Math.cos(startLat * Math.PI / 180);
  return { x: Math.round(xOffset), y: Math.round(yOffset) };
}

function locationSuccess(pos) {
  if (!isTracking) return;

  var lat = pos.coords.latitude;
  var lon = pos.coords.longitude;
  var alt = pos.coords.altitude; 
  var acc = pos.coords.accuracy;

  var accStr = "";
  if (acc > 99) {
    accStr = "Acc " + (acc / 1000).toFixed(1) + "km";
  } else {
    accStr = "Acc " + Math.round(acc) + "m";
  }
  
  // Set showOnWatch to true for Accuracy updates
  sendDebug(accStr, true);

  if (startLat === null) {
    startLat = lat;
    startLon = lon;
    lastLat = lat;
    lastLon = lon;
  }

  totalDistanceMeters += calculateDistance(lastLat, lastLon, lat, lon);
  lastLat = lat;
  lastLon = lon;
  
  if (alt !== null) {
    if (lastAlt !== null) {
      var diff = alt - lastAlt;
      if (diff > 0) totalAscent += diff;
      else if (diff < 0) totalDescent += Math.abs(diff);
    }
    lastAlt = alt;
  }

  var offsets = getMeterOffsets(lat, lon);

  var speedMPS = pos.coords.speed;
  if (!speedMPS || speedMPS < 0) speedMPS = 0;
  var speedKmHScaled = Math.round(speedMPS * 3.6 * 10);

  pointsCollectedInSession++;

  if (pointsCollectedInSession === 12) {
    // Hidden from watch
    sendDebug("Normal Power (15s)", false); 
    restartTrackingWithNewInterval(15000);
  }

  var payload = {
    'KEY_NEW_POINT_X': offsets.x,
    'KEY_NEW_POINT_Y': offsets.y,
    'KEY_DISTANCE': Math.round(totalDistanceMeters),
    'KEY_SPEED': speedKmHScaled,
    'KEY_ASCENT': Math.round(totalAscent),
    'KEY_DESCENT': Math.round(totalDescent)
  };

  Pebble.sendAppMessage(payload);
}

function locationError(err) {
  var errorMsg = "ERR: ";
  if (err.code === 1) errorMsg += "No Permission";
  else if (err.code === 2) errorMsg += "Position Unavail";
  else if (err.code === 3) errorMsg += "Timeout";
  else errorMsg += err.message;
  
  // Kept true so you still get a visual warning if GPS drops entirely
  sendDebug(errorMsg, true); 
}

function startTrackingLocation(intervalTime) {
  if (watchPositionId !== null) {
    navigator.geolocation.clearWatch(watchPositionId);
  }

  watchPositionId = navigator.geolocation.watchPosition(locationSuccess, locationError, {
    enableHighAccuracy: true,
    maximumAge: (intervalTime === 5000) ? 0 : intervalTime, 
    timeout: 30000 
  });
}

function restartTrackingWithNewInterval(newInterval) {
  if (!isTracking) return;
  startTrackingLocation(newInterval);
}

Pebble.addEventListener('appmessage', function(e) {
  var state = e.payload['KEY_STATE'];

  if (typeof state !== 'undefined') {
    if (state === 1) { 
      isTracking = true;
      if (watchPositionId === null) {
        pointsCollectedInSession = 0;
        sendDebug("Acquiring GPS...", false); // Hidden from watch
        startTrackingLocation(5000);
      }
    } else if (state === 0) { 
      isTracking = false;
      sendDebug("Paused.", false); // Hidden from watch
    } else if (state === 2) { 
      isTracking = false;
      if (watchPositionId !== null) {
        navigator.geolocation.clearWatch(watchPositionId);
        watchPositionId = null;
      }
      startLat = null; startLon = null;
      totalDistanceMeters = 0; totalAscent = 0; totalDescent = 0; lastAlt = null;
      pointsCollectedInSession = 0;
    }
  }
});

Pebble.addEventListener('ready', function() {
  sendDebug("Ready.", false); // Hidden from watch
});