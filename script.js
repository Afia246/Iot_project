const $ = (id) => document.getElementById(id);

let port = null;
let reader = null;
let keepReading = false;
let inputBuffer = "";
let packetCount = 0;

let pwm = 0;
let running = false;
let dir = "Stopped";

Chart.defaults.color = "#9eb0c2";
Chart.defaults.font.size = 13;
Chart.defaults.borderColor = "#192b3e";

const labels = Array.from({ length: 40 }, (_, i) => i);

const chartOptions = {
  responsive: true,
  maintainAspectRatio: false,
  animation: false,
  scales: {
    x: { display: false },
    y: { grid: { color: "#192b3e" } }
  }
};

const rpmChart = new Chart($("rpmChart"), {
  type: "line",
  data: {
    labels,
    datasets: [{
      label: "RPM",
      data: Array(40).fill(0),
      borderColor: "#368cff",
      backgroundColor: "#368cff22",
      fill: true,
      pointRadius: 0,
      tension: 0.35,
      borderWidth: 2
    }]
  },
  options: {
    ...chartOptions,
    plugins: { legend: { display: false } }
  }
});

const vibChart = new Chart($("vibChart"), {
  type: "line",
  data: {
    labels,
    datasets: [
      {
        label: "X RMS",
        data: Array(40).fill(0),
        borderColor: "#368cff",
        pointRadius: 0,
        tension: 0.35,
        borderWidth: 2
      },
      {
        label: "Y RMS",
        data: Array(40).fill(0),
        borderColor: "#31d870",
        pointRadius: 0,
        tension: 0.35,
        borderWidth: 2
      },
      {
        label: "Z RMS",
        data: Array(40).fill(0),
        borderColor: "#a04dff",
        pointRadius: 0,
        tension: 0.35,
        borderWidth: 2
      }
    ]
  },
  options: chartOptions
});

const axisChart = new Chart($("fftChart"), {
  type: "bar",
  data: {
    labels: ["X RMS", "Y RMS", "Z RMS"],
    datasets: [{
      label: "m/s²",
      data: [0, 0, 0],
      backgroundColor: ["#368cff", "#31d870", "#a04dff"],
      borderRadius: 5
    }]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { grid: { display: false } },
      y: { grid: { color: "#192b3e" } }
    }
  }
});

function pushPoint(chart, datasetIndex, value) {
  const data = chart.data.datasets[datasetIndex].data;
  data.push(value);
  data.shift();
}

function setConnectionState(connected, message = "") {
  $("connectionStatus").textContent = connected ? "● CONNECTED" : "● DISCONNECTED";
  $("connectionStatus").style.color = connected ? "#32dc72" : "#ffb832";
  $("serialState").textContent = connected ? "Connected" : "Not connected";
  $("portInfo").textContent = connected ? "Open" : "Closed";
  $("connectBtn").textContent = connected ? "DISCONNECT" : "CONNECT ESP32";

  if (message) {
    $("alarm2").textContent = message;
  }
}

async function connectSerial() {
  if (!("serial" in navigator)) {
    alert("Web Serial is not supported in this browser. Use Google Chrome or Microsoft Edge.");
    return;
  }

  if (port) {
    await disconnectSerial();
    return;
  }

  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });

    keepReading = true;
    setConnectionState(true, "ESP32 serial connection established.");
    $("alarm").textContent = "☑ ESP32 connected";
    $("alarm").className = "ok";

    readSerialLoop();
  } catch (error) {
    console.error(error);
    port = null;
    setConnectionState(false, "Could not open the ESP32 serial port.");
  }
}

async function disconnectSerial() {
  keepReading = false;

  try {
    if (reader) {
      await reader.cancel();
    }
  } catch (e) {}

  try {
    if (port) {
      await port.close();
    }
  } catch (e) {}

  reader = null;
  port = null;
  setConnectionState(false, "ESP32 disconnected.");
}

async function readSerialLoop() {
  const decoder = new TextDecoder();

  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();

    try {
      while (keepReading) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;

        inputBuffer += decoder.decode(value, { stream: true });

        let newlineIndex;
        while ((newlineIndex = inputBuffer.indexOf("\n")) >= 0) {
          const line = inputBuffer.slice(0, newlineIndex).trim();
          inputBuffer = inputBuffer.slice(newlineIndex + 1);

          if (line) parseSerialLine(line);
        }
      }
    } catch (error) {
      console.error("Serial read error:", error);
    } finally {
      reader.releaseLock();
      reader = null;
    }
  }
}

function parseSerialLine(line) {
  console.log("ESP32:", line);

  // Expected format:
  // DATA,period_us,rpm,ax_rms,ay_rms,az_rms,overall_rms,temp_c,prediction,duty,direction
  if (!line.startsWith("DATA,")) return;

  const parts = line.split(",");
  if (parts.length < 11) return;

  const periodUs = Number(parts[1]);
  const rpm = Number(parts[2]);
  const axRms = Number(parts[3]);
  const ayRms = Number(parts[4]);
  const azRms = Number(parts[5]);
  const overallRms = Number(parts[6]);
  const tempC = Number(parts[7]);
  const prediction = parts[8].trim().toLowerCase();
  const duty = Number(parts[9]);
  const direction = parts[10].trim();

  packetCount++;
  $("packetCount").textContent = packetCount.toLocaleString();
  $("time").textContent = new Date().toLocaleTimeString();

  $("rpm").textContent = Number.isFinite(rpm) ? Math.round(rpm).toLocaleString() : "--";
  $("periodKpi").textContent = Number.isFinite(periodUs) ? periodUs.toFixed(0) : "--";
  $("periodInfo").textContent = Number.isFinite(periodUs) ? `${periodUs.toFixed(1)} µs` : "-- µs";

  $("xRms").textContent = `${axRms.toFixed(3)} m/s²`;
  $("yRms").textContent = `${ayRms.toFixed(3)} m/s²`;
  $("zRms").textContent = `${azRms.toFixed(3)} m/s²`;
  $("overallRms").textContent = `${overallRms.toFixed(3)} m/s²`;
  $("vibOverall").textContent = overallRms.toFixed(3);

  $("tempKpi").textContent = tempC.toFixed(1);
  $("temperature").textContent = `♨　${tempC.toFixed(1)}°C`;

  updatePrediction(prediction);

  pwm = Math.round((duty / 255) * 100);
  running = duty > 0 && direction.toLowerCase() !== "stopped";
  dir = direction;

  $("pwm").value = pwm;
  syncMotorUi();

  pushPoint(rpmChart, 0, rpm);
  rpmChart.update();

  pushPoint(vibChart, 0, axRms);
  pushPoint(vibChart, 1, ayRms);
  pushPoint(vibChart, 2, azRms);
  vibChart.update();

  axisChart.data.datasets[0].data = [axRms, ayRms, azRms];
  axisChart.update();
}

function updatePrediction(prediction) {
  const p = prediction.toLowerCase();
  const predEl = $("prediction");
  const condition = $("conditionKpi");

  predEl.textContent = p.toUpperCase();
  condition.textContent = p.toUpperCase();
  $("evPred").textContent = p.toUpperCase();

  if (p === "normal") {
    $("healthCircle").textContent = "OK";
    $("healthLabel").textContent = "Normal";
    $("risk").textContent = "LOW";
    $("alarm").textContent = "☑ Motor condition normal";
    $("alarm").className = "ok";
    predEl.style.color = "#32dd72";
  } else if (p === "abnormal") {
    $("healthCircle").textContent = "!";
    $("healthLabel").textContent = "Abnormal";
    $("risk").textContent = "HIGH";
    $("alarm").textContent = "⚠ Abnormal condition detected";
    $("alarm").className = "";
    predEl.style.color = "#ffb832";
  } else {
    $("healthCircle").textContent = "--";
    $("healthLabel").textContent = "Unknown";
    $("risk").textContent = "--";
    predEl.style.color = "#eef6ff";
  }
}

async function sendCommand(command) {
  if (!port || !port.writable) {
    alert("Connect the ESP32 first.");
    return;
  }

  const writer = port.writable.getWriter();

  try {
    await writer.write(new TextEncoder().encode(command));
  } finally {
    writer.releaseLock();
  }
}

function syncMotorUi() {
  const p = `${pwm}%`;
  $("pwmText").textContent = p;
  $("duty").textContent = p;
  $("evSpeed").textContent = p;
  $("direction").textContent = dir;
  $("evDir").textContent = dir;
  $("motor").textContent = running ? "RUNNING ●" : "STOPPED ●";
  $("motor").className = "state" + (running ? "" : " danger");
}

async function go(direction) {
  if (!port) {
    alert("Connect the ESP32 first.");
    return;
  }

  if (pwm === 0) {
    pwm = 50;
    $("pwm").value = pwm;
    const level = Math.round((pwm / 100) * 9);
    await sendCommand(String(level));
  }

  await sendCommand(direction === "Forward" ? "f" : "r");
  dir = direction;
  running = true;
  syncMotorUi();
}

async function stopM() {
  if (!port) {
    alert("Connect the ESP32 first.");
    return;
  }

  await sendCommand("s");
  pwm = 0;
  running = false;
  dir = "Stopped";
  $("pwm").value = 0;
  syncMotorUi();
}

$("pwm").addEventListener("input", (event) => {
  pwm = Number(event.target.value);
  $("pwmText").textContent = `${pwm}%`;
});

$("pwm").addEventListener("change", async (event) => {
  pwm = Number(event.target.value);
  const level = Math.round((pwm / 100) * 9);
  await sendCommand(String(level));
});

$("connectBtn").addEventListener("click", connectSerial);

window.addEventListener("beforeunload", () => {
  keepReading = false;
});

setConnectionState(false);
syncMotorUi();
