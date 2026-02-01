// ---------- Firebase SDK ----------
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.7.1/firebase-app.js";
import {
  getFirestore, doc, onSnapshot, updateDoc
} from "https://www.gstatic.com/firebasejs/10.7.1/firebase-firestore.js";
import {
  getAuth, signInAnonymously
} from "https://www.gstatic.com/firebasejs/10.7.1/firebase-auth.js";

// ---------- Firebase Config ----------
const firebaseConfig = {
  apiKey: "Your_Project_APIKey",
  authDomain: "Your_Domain",
  projectId: "Your Project ID"
};

const app = initializeApp(firebaseConfig);
const db = getFirestore(app);
const auth = getAuth(app);

// ---------- HTML Elements ----------
const bpmEl = document.getElementById("bpm");
const spo2El = document.getElementById("spo2");
const moveEl = document.getElementById("move");
const stressEl = document.getElementById("stress");
const sleepEl = document.getElementById("sleep");
const sosEl = document.getElementById("sos");
const locationLink = document.getElementById("locationLink");

// ---------- SOS Trigger State ----------
let sosTriggered = false;

// ---------- Sign in anonymously ----------
signInAnonymously(auth)
  .then(() => {
    console.log("✅ Signed in anonymously");

    // ---------- Firestore Live Listener ----------
    const docRef = doc(db, "SmartBand", "User1");

    onSnapshot(docRef, (docSnap) => {
      if (!docSnap.exists()) return;

      const d = docSnap.data();

      // Update UI
      bpmEl.innerText = d.BPM ?? "--";
      spo2El.innerText = d.SpO2 ?? "--";
      moveEl.innerText = d.Movement ? d.Movement.toFixed(2) : "--";
      stressEl.innerText = ["Relax", "Normal", "High"][d.Stress] ?? "--";
      sleepEl.innerText = d.Sleep ? "YES" : "NO";
      sosEl.innerText = d.SOS ? "ON" : "OFF";

      // ---------- SOS Trigger ----------
      if (d.SOS === true && !sosTriggered) {
        sosTriggered = true;
        triggerSOS();
      }

      if (d.SOS === false) {
        sosTriggered = false;
        sosEl.style.color = "white";
        locationLink.style.display = "none";
      }
    });
  })
  .catch((err) => {
    console.error("Auth Error:", err);
    alert("Firestore access failed: " + err.message);
  });


// =======================================================
//                  SOS FUNCTION
// =======================================================

function triggerSOS() {
  sosEl.style.color = "red";

  // Blink SOS text
  let blink = true;
  const blinkInterval = setInterval(() => {
    sosEl.style.color = blink ? "red" : "white";
    blink = !blink;
  }, 500);

  // Stop blinking after 10 seconds
  setTimeout(() => clearInterval(blinkInterval), 10000);

  // ---------- Get Phone GPS ----------
  navigator.geolocation.getCurrentPosition(
    async (pos) => {
      const lat = pos.coords.latitude;
      const lon = pos.coords.longitude;
      const mapLink = `https://maps.google.com/?q=${lat},${lon}`;

      // Show map link on page
      locationLink.style.display = "block";
      locationLink.href = mapLink;
      locationLink.innerText = "📍 Open Live Location";

      // ---------- Save Phone Location to Firestore ----------
      const docRef = doc(db, "SmartBand", "User1");
      await updateDoc(docRef, {
        PhoneLat: lat,
        PhoneLon: lon,
        PhoneMap: mapLink
      });

      // ---------- Auto Open WhatsApp ----------
      const emergencyNumber = "91xxxxxxxx"; // <-- Change if needed
      const message = `🚨 SOS ALERT!\nUser needs help!\nLive Location:\n${mapLink}`;

      // Open WhatsApp in NEW TAB without closing website
window.open(
  `https://wa.me/${emergencyNumber}?text=${encodeURIComponent(message)}`,
  "_blank"
);

    },

    (err) => {
      console.log("GPS Error:", err);
      alert("Location permission denied. Cannot send SOS location.");
    },

    { enableHighAccuracy: true, timeout: 10000 }
  );
}

