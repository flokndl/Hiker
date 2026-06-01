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

var currentHikePoints = [];
var hikeStartTime = null;

// --- NATIVE BLUETOOTH MESSAGE QUEUE ---
// This ensures we never overwhelm the Pebble's tiny memory buffer when streaming huge files!
var msgQueue = [];
var isSending = false;

function sendNextMessage() {
  if (msgQueue.length === 0) {
    isSending = false;
    return;
  }
  isSending = true;
  var msg = msgQueue[0];
  
  Pebble.sendAppMessage(msg, function() {
    msgQueue.shift(); // Success! Remove from queue and send next
    sendNextMessage();
  }, function(e) {
    console.log("Queue retrying..."); // Failed, leave in queue and try again
    setTimeout(sendNextMessage, 50);
  });
}

function queueMessage(dict) {
  msgQueue.push(dict);
  if (!isSending) sendNextMessage();
}

// --- APP LOGIC ---
function sendDebug(msg, showOnWatch) {
  console.log("DEBUG: " + msg);
  queueMessage({
    'KEY_DEBUG_MSG': showOnWatch ? msg.substring(0, 60) : ""
  });
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
  currentHikePoints.push([offsets.x, offsets.y]);

  var speedMPS = pos.coords.speed;
  if (!speedMPS || speedMPS < 0) speedMPS = 0;
  var speedKmHScaled = Math.round(speedMPS * 3.6 * 10);

  pointsCollectedInSession++;

  if (pointsCollectedInSession === 12) {
    sendDebug("Normal Power (15s)", false); 
    restartTrackingWithNewInterval(15000);
  }

  queueMessage({
    'KEY_NEW_POINT_X': offsets.x,
    'KEY_NEW_POINT_Y': offsets.y,
    'KEY_DISTANCE': Math.round(totalDistanceMeters),
    'KEY_SPEED': speedKmHScaled,
    'KEY_ASCENT': Math.round(totalAscent),
    'KEY_DESCENT': Math.round(totalDescent)
  });
}

function locationError(err) {
  var errorMsg = "ERR: ";
  if (err.code === 1) errorMsg += "No Permission";
  else if (err.code === 2) errorMsg += "Position Unavail";
  else if (err.code === 3) errorMsg += "Timeout";
  else errorMsg += err.message;
  
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
  var dict = e.payload;
  var state = dict['KEY_STATE'];
  var cmdGetCount = dict['CMD_GET_COUNT'];
  var cmdGetItem = dict['CMD_GET_ITEM'];
  var cmdGetHike = dict['CMD_GET_HIKE']; // NEW: Replay Trigger

  // --- REPLAY LOGIC ---
  if (typeof cmdGetHike !== 'undefined') {
    var history = JSON.parse(localStorage.getItem('hiker_history')) || [];
    var realIndex = history.length - 1 - cmdGetHike; 
    
    if (realIndex >= 0 && realIndex < history.length) {
      var hike = history[realIndex];
      
      // Step 1: Send the Start Signal & Summary Stats
      queueMessage({
        'REPLAY_START': 1,
        'KEY_DISTANCE': hike.distance,
        'KEY_SPEED': 0, 
        'KEY_ASCENT': hike.ascent,
        'KEY_DESCENT': hike.descent,
        'REPLAY_TIME': hike.duration
      });

      // Step 2: Stream all the points!
      for (var i = 0; i < hike.points.length; i++) {
        queueMessage({
          'REPLAY_PT_X': hike.points[i][0],
          'REPLAY_PT_Y': hike.points[i][1]
        });
      }
    }
  }

  // --- HISTORY MENU LOGIC ---
  if (typeof cmdGetCount !== 'undefined') {
    var history = JSON.parse(localStorage.getItem('hiker_history')) || [];
    var count = Math.min(history.length, 10); 
    queueMessage({ 'LIST_COUNT': count });
  }

  if (typeof cmdGetItem !== 'undefined') {
    var history = JSON.parse(localStorage.getItem('hiker_history')) || [];
    var realIndex = history.length - 1 - cmdGetItem; 
    
    if (realIndex >= 0 && realIndex < history.length) {
      var hike = history[realIndex];
      var d = new Date(hike.id);
      
      // NEW FIX: Custom Date Formatter ("12 Jun '26")
      var months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
      var monthStr = months[d.getMonth()];
      var yearStr = String(d.getFullYear()).slice(-2);
      var title = d.getDate() + " " + monthStr + " '" + yearStr; 

      var subtitle = "Distance: " + (hike.distance / 1000).toFixed(2) + " km";

      queueMessage({
        'ITEM_INDEX': cmdGetItem,
        'ITEM_TITLE': title,
        'ITEM_SUBTITLE': subtitle
      });
    }
  }

  // --- LIVE TRACKING LOGIC ---
  if (typeof state !== 'undefined') {
    if (state === 1) { 
      isTracking = true;
      hikeStartTime = Date.now();
      currentHikePoints = [];

      if (watchPositionId === null) {
        pointsCollectedInSession = 0;
        sendDebug("Acquiring GPS...", false); 
        startTrackingLocation(5000);
      }
    } else if (state === 0) { 
      isTracking = false;
      sendDebug("Paused.", false); 
    } else if (state === 2) { 
      isTracking = false;

      if (hikeStartTime !== null && currentHikePoints.length > 0) {
        var durationSecs = Math.round((Date.now() - hikeStartTime) / 1000);
        
        var completedHike = {
          id: Date.now(), 
          duration: durationSecs,
          distance: Math.round(totalDistanceMeters),
          ascent: Math.round(totalAscent),
          descent: Math.round(totalDescent),
          points: currentHikePoints
        };

        var history = JSON.parse(localStorage.getItem('hiker_history')) || [];
        history.push(completedHike);

        try {
          localStorage.setItem('hiker_history', JSON.stringify(history));
          console.log("Mega Success! Hike archived. Total hikes saved: " + history.length);
        } catch (error) {
          console.log("ERR: Phone storage limit reached!");
        }
      }

      if (watchPositionId !== null) {
        navigator.geolocation.clearWatch(watchPositionId);
        watchPositionId = null;
      }
      startLat = null; startLon = null;
      totalDistanceMeters = 0; totalAscent = 0; totalDescent = 0; lastAlt = null;
      pointsCollectedInSession = 0;
      hikeStartTime = null;
      currentHikePoints = [];
    }
  }
});

Pebble.addEventListener('ready', function() {
  sendDebug("Ready.", false); 
});