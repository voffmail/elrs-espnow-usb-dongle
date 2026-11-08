package com.elrs.dongle.stub

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.net.wifi.WifiManager
import androidx.core.app.ServiceCompat
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

class DongleService : Service() {

    companion object {
        const val EXTRA_DEVICE_NAME = "device_name"
        const val EXTRA_PHRASE = "phrase"
        const val EXTRA_CHANNEL = "channel"
        const val EXTRA_UID = "uid"
        const val ACTION_STOP = "com.elrs.dongle.stub.STOP"
        const val ACTION_UPDATE_TARGET = "com.elrs.dongle.stub.UPDATE_TARGET"
        const val ACTION_APPLY_CONFIG = "com.elrs.dongle.stub.APPLY_CONFIG"
        const val ACTION_APPLY_UID = "com.elrs.dongle.stub.APPLY_UID"
        const val ACTION_READ_CONFIG = "com.elrs.dongle.stub.READ_CONFIG"
        const val ACTION_RESET_CONFIG = "com.elrs.dongle.stub.RESET_CONFIG"
        private const val CHANNEL_ID = "dongle_v2"
        private const val NOTIF_ID = 1
        private const val PREFS = "dongle"
        private const val PREF_DEVICE = "device"

        @Volatile
        var status: String = "idle"
            private set

        @Volatile
        var configStatus: String = "config: -"
            private set

        @Volatile
        var configInfo: DongleConfig.ConfigInfo? = null
            private set

        @Volatile
        var configRevision: Long = 0L
            private set
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private var worker: Thread? = null
    private var statusExec: ScheduledExecutorService? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    @Volatile private var running = false
    @Volatile private var framer: CrsfFramer? = null
    @Volatile private var netOut: NetOut? = null
    @Volatile private var connectionStatus = "idle"
    @Volatile private var lastNotificationText: String? = null
    private val pendingCommand = AtomicReference<ByteArray?>(null)
    private val configParser = DongleConfig.Parser()

    override fun onCreate() {
        super.onCreate()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java)
            val channel = NotificationChannel(
                CHANNEL_ID,
                "ELRS dongle",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                setSound(null, null)
                enableVibration(false)
                setShowBadge(false)
                lockscreenVisibility = Notification.VISIBILITY_SECRET
            }
            nm.createNotificationChannel(channel)
            // Remove the old noisy channel from previous builds.
            nm.deleteNotificationChannel("dongle")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }

        when (intent?.action) {
            ACTION_UPDATE_TARGET -> {
                netOut?.reload()
                return if (running) START_STICKY else START_NOT_STICKY
            }
            ACTION_APPLY_CONFIG, ACTION_APPLY_UID, ACTION_READ_CONFIG,
            ACTION_RESET_CONFIG -> {
                if (!running) {
                    configStatus = "start dongle first"
                    return START_NOT_STICKY
                }
                val command = when (intent.action) {
                    ACTION_APPLY_CONFIG -> DongleConfig.buildSetConfig(
                        intent.getStringExtra(EXTRA_PHRASE).orEmpty(),
                        intent.getIntExtra(EXTRA_CHANNEL, 1)
                    )
                    ACTION_APPLY_UID -> {
                        val uid = intent.getByteArrayExtra(EXTRA_UID)
                        if (uid == null || uid.size != 6 ||
                            DongleConfig.uidIsZero(DongleConfig.normalizeUid(uid))) {
                            configStatus = "invalid UID"
                            return START_NOT_STICKY
                        }
                        DongleConfig.buildSetUid(
                            uid, intent.getIntExtra(EXTRA_CHANNEL, 1)
                        )
                    }
                    ACTION_READ_CONFIG -> DongleConfig.buildGetConfig()
                    else -> DongleConfig.buildReset()
                }
                pendingCommand.set(command)
                configStatus = "command queued"
                return START_STICKY
            }
        }

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val deviceName = intent?.getStringExtra(EXTRA_DEVICE_NAME)
            ?: prefs.getString(PREF_DEVICE, null)

        if (deviceName == null) {
            connectionStatus = "no device selected"
            status = connectionStatus
            stopSelf()
            return START_NOT_STICKY
        }
        prefs.edit().putString(PREF_DEVICE, deviceName).apply()

        connectionStatus = "starting"
        startForegroundCompat(connectionStatus)
        acquireWakeLock()

        if (!running) {
            running = true
            worker = Thread({ runDongle(deviceName) }, "dongle-usb").apply { start() }
            startStatusUpdater()
        }

        return START_STICKY
    }

    private fun runDongle(deviceName: String) {
        val net = NetOut(this)
        netOut = net
        configInfo = null

        val framerLocal = CrsfFramer { frame -> net.sendRaw(frame) }
        framer = framerLocal

        while (running) {
            try {
                val usb = getSystemService(UsbManager::class.java)
                val driver = UsbSerialProber.getDefaultProber()
                    .findAllDrivers(usb)
                    .firstOrNull { it.device.deviceName == deviceName }
                    ?: UsbSerialProber.getDefaultProber().findAllDrivers(usb).firstOrNull()

                if (driver == null) {
                    connectionStatus = "no supported USB serial device"
                    sleep(2000)
                    continue
                }

                val connection = usb.openDevice(driver.device)
                if (connection == null) {
                    connectionStatus = "USB open failed (permission?)"
                    sleep(1000)
                    continue
                }

                val port = driver.ports[0]
                try {
                    port.open(connection)
                    port.setParameters(
                        115200,
                        8,
                        UsbSerialPort.STOPBITS_1,
                        UsbSerialPort.PARITY_NONE
                    )
                    // Keep the DevKitM-1 auto-reset lines idle.
                    try {
                        port.setDTR(false)
                        port.setRTS(false)
                    } catch (_: IOException) {
                    }

                    connectionStatus = "connected"
                    configParser.reset()
                    var lastConfigRequest = 0L
                    val buf = ByteArray(512)
                    while (running) {
                        // Auto-read the dongle config after connect. Retry until
                        // a response arrives; opening a native-USB port may
                        // reset the board once.
                        if (configInfo == null &&
                            SystemClock.elapsedRealtime() - lastConfigRequest >= 2000) {
                            if (pendingCommand.compareAndSet(null, DongleConfig.buildGetConfig())) {
                                lastConfigRequest = SystemClock.elapsedRealtime()
                            }
                        }
                        pendingCommand.getAndSet(null)?.let { command ->
                            try {
                                port.write(command, 1000)
                            } catch (e: IOException) {
                                configStatus = "write failed: ${e.message}"
                            }
                        }
                        val n = try {
                            port.read(buf, 200)
                        } catch (_: IOException) {
                            -1
                        }
                        if (n < 0) break
                        if (n > 0) {
                            framerLocal.feed(buf, n)
                            for (frame in configParser.feed(buf, n)) {
                                handleConfigFrame(frame)
                            }
                        }
                    }
                } finally {
                    try {
                        port.close()
                    } catch (_: Exception) {
                    }
                    try {
                        connection.close()
                    } catch (_: Exception) {
                    }
                }

                if (running) connectionStatus = "disconnected, retrying"
            } catch (t: Throwable) {
                connectionStatus = "usb: ${t.message}"
            }

            sleep(1000)
        }

        net.close()
    }

    private fun handleConfigFrame(frame: DongleConfig.Frame) {
        when (frame.type) {
            DongleConfig.GET_CONFIG or DongleConfig.RESPONSE -> {
                val info = DongleConfig.parseConfig(frame.payload)
                if (info != null) {
                    configInfo = info
                    configRevision++
                }
                configStatus = "config: " + DongleConfig.decodeConfig(frame.payload)
            }
            DongleConfig.SET_CONFIG or DongleConfig.RESPONSE -> {
                configStatus = "set: " + DongleConfig.decodeAck(frame.payload)
                if (frame.payload.isNotEmpty() &&
                    (frame.payload[0].toInt() and 0xFF) == DongleConfig.OK) {
                    pendingCommand.compareAndSet(null, DongleConfig.buildGetConfig())
                }
            }
            DongleConfig.SET_UID or DongleConfig.RESPONSE -> {
                configStatus = "set UID: " + DongleConfig.decodeAck(frame.payload)
                if (frame.payload.isNotEmpty() &&
                    (frame.payload[0].toInt() and 0xFF) == DongleConfig.OK) {
                    pendingCommand.compareAndSet(null, DongleConfig.buildGetConfig())
                }
            }
            DongleConfig.RESET or DongleConfig.RESPONSE -> {
                configStatus = "reset: " + DongleConfig.decodeAck(frame.payload)
                if (frame.payload.isNotEmpty() &&
                    (frame.payload[0].toInt() and 0xFF) == DongleConfig.OK) {
                    pendingCommand.compareAndSet(null, DongleConfig.buildGetConfig())
                }
            }
            DongleConfig.GET_STATS or DongleConfig.RESPONSE ->
                configStatus = "stats: " + DongleConfig.decodeStats(frame.payload)
            DongleConfig.PING or DongleConfig.RESPONSE ->
                configStatus = "ping: " + DongleConfig.decodeAck(frame.payload)
            else -> configStatus =
                "config: unknown response 0x${frame.type.toString(16)}"
        }
    }

    private fun startStatusUpdater() {
        val exec = Executors.newSingleThreadScheduledExecutor()
        statusExec = exec
        exec.scheduleAtFixedRate({
            status = buildStatusText()
            // Keep the notification quiet: update it only when the state or
            // target changes, not every second. Counters stay in the app UI.
            notifyStatus(buildNotificationText())
        }, 0, 1, TimeUnit.SECONDS)
    }

    private fun buildStatusText(): String {
        val f = framer
        val n = netOut
        val frames = f?.frames ?: 0
        val crc = f?.crcErrors ?: 0
        val raw = n?.rawPackets ?: 0
        val type = f?.lastType ?: -1
        val len = f?.lastLen ?: 0
        val last = if (type >= 0) {
            "0x" + type.toString(16).uppercase().padStart(2, '0') + "/$len"
        } else {
            "-"
        }
        val netText = n?.status() ?: "net: -"
        return "$connectionStatus | crsf=$frames crc=$crc last=$last | raw=$raw | $netText"
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "ELRS:DongleService"
        ).apply {
            setReferenceCounted(false)
            acquire()
        }
        val wm = applicationContext.getSystemService(WifiManager::class.java)
        if (wm != null && wifiLock?.isHeld != true) {
            wifiLock = wm.createWifiLock(
                WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                "ELRS:DongleWifi"
            ).apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    private fun startForegroundCompat(text: String) {
        val notification = buildNotification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ServiceCompat.startForeground(
                this,
                NOTIF_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } else {
            startForeground(NOTIF_ID, notification)
        }
    }

    private fun buildNotificationText(): String {
        val target = netOut?.status() ?: "net: -"
        return "$connectionStatus | $target"
    }

    private fun notifyStatus(text: String) {
        if (text == lastNotificationText) return
        lastNotificationText = text
        mainHandler.post { updateNotification(text) }
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIF_ID, buildNotification(text))
    }

    private fun buildNotification(text: String): Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            Notification.Builder(this)
        }

        return builder
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("ELRS dongle")
            .setContentText(text)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setPriority(Notification.PRIORITY_LOW)
            .setVisibility(Notification.VISIBILITY_SECRET)
            .build()
    }

    private fun sleep(ms: Long) {
        try {
            Thread.sleep(ms)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
    }

    override fun onDestroy() {
        running = false
        statusExec?.shutdownNow()
        statusExec = null
        worker?.interrupt()
        worker = null
        netOut?.close()
        netOut = null
        try {
            wakeLock?.release()
        } catch (_: Exception) {
        }
        wakeLock = null
        try {
            wifiLock?.release()
        } catch (_: Exception) {
        }
        wifiLock = null
        connectionStatus = "stopped"
        status = "stopped"
        lastNotificationText = null
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
