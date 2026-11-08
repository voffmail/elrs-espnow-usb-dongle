package com.elrs.dongle.stub

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.widget.doAfterTextChanged
import com.hoho.android.usbserial.driver.UsbSerialProber

class MainActivity : Activity() {

    companion object {
        private const val ACTION_USB_PERMISSION = "com.elrs.dongle.stub.USB_PERMISSION"
        private const val REQ_NOTIFICATIONS = 100
        private const val PREFS = "dongle"
        private const val PREF_TARGET_HOST = "target_host"
        private const val PREF_TARGET_PORT = "target_port"
        private const val FIXED_CHANNEL = 1
    }

    private lateinit var statusView: TextView
    private lateinit var configView: TextView
    private lateinit var detailView: TextView
    private lateinit var phraseView: EditText
    private lateinit var manualUidView: CheckBox
    private lateinit var uidView: EditText
    private lateinit var targetView: EditText
    private lateinit var streamButton: Button
    private lateinit var applyConfigButton: Button

    private val handler = Handler(Looper.getMainLooper())
    private var destroyed = false
    private var pendingDevice: UsbDevice? = null
    private var loadingConfigFields = false
    private var configFieldsDirty = false
    private var forceConfigFill = false
    private var lastConfigRevision = -1L

    private val poll = object : Runnable {
        override fun run() {
            if (destroyed) return
            statusView.text = DongleService.status
            configView.text = DongleService.configStatus
            streamButton.text = if (isStreaming()) {
                getString(R.string.stop_stream)
            } else {
                getString(R.string.start_stream)
            }
            applyConfigFromDongle()
            pendingDevice?.let { device ->
                val usb = getSystemService(UsbManager::class.java)
                if (usb.hasPermission(device)) {
                    pendingDevice = null
                    startDongle(device)
                }
            }
            handler.postDelayed(this, 1000)
        }
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != ACTION_USB_PERMISSION) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            val device: UsbDevice? = if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            }
            if (granted && device != null) {
                pendingDevice = null
                startDongle(device)
            } else {
                statusView.text = "USB permission denied"
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 64, 48, 48)
        }

        val title = TextView(this).apply {
            text = getString(R.string.title)
            textSize = 22f
        }

        statusView = TextView(this).apply {
            text = DongleService.status
            textSize = 15f
            setPadding(0, 24, 0, 8)
        }

        configView = TextView(this).apply {
            text = DongleService.configStatus
            textSize = 13f
            setPadding(0, 0, 0, 8)
        }

        phraseView = EditText(this).apply {
            hint = getString(R.string.phrase_hint)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }

        phraseView.doAfterTextChanged {
            if (!loadingConfigFields) configFieldsDirty = true
        }

        manualUidView = CheckBox(this).apply {
            text = getString(R.string.manual_uid)
            isChecked = false
            setOnCheckedChangeListener { _, _ ->
                if (!loadingConfigFields) configFieldsDirty = true
                updateModeFields()
                updateApplyEnabled()
            }
        }

        uidView = EditText(this).apply {
            hint = getString(R.string.uid_hint)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }
        uidView.doAfterTextChanged {
            if (!loadingConfigFields) configFieldsDirty = true
            updateApplyEnabled()
        }
        updateModeFields()

        targetView = EditText(this).apply {
            hint = getString(R.string.target_hint)
            inputType = InputType.TYPE_CLASS_TEXT
        }

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val savedHost = prefs.getString(PREF_TARGET_HOST, NetOut.DEFAULT_TARGET_HOST).orEmpty()
        val savedPort = prefs.getInt(PREF_TARGET_PORT, NetOut.DEFAULT_PORT)
        targetView.setText(if (savedHost.isEmpty()) "" else "$savedHost:$savedPort")

        applyConfigButton = Button(this).apply {
            text = getString(R.string.apply_config)
            setOnClickListener { applyConfig() }
        }

        val resetConfig = Button(this).apply {
            text = getString(R.string.reset_config)
            setOnClickListener { sendConfigAction(DongleService.ACTION_RESET_CONFIG) }
        }

        val applyTargetButton = Button(this).apply {
            text = getString(R.string.apply_target)
            setOnClickListener { applyTarget() }
        }

        streamButton = Button(this).apply {
            text = getString(R.string.start_stream)
            setOnClickListener {
                if (isStreaming()) stopDongle() else connectDongle()
            }
        }

        detailView = TextView(this).apply {
            text = ""
            textSize = 13f
            setPadding(0, 8, 0, 24)
        }

        layout.addView(title)
        layout.addView(statusView)
        layout.addView(configView)
        layout.addView(manualUidView)
        layout.addView(phraseView)
        layout.addView(uidView)
        layout.addView(applyConfigButton)
        layout.addView(resetConfig)
        layout.addView(targetView)
        layout.addView(applyTargetButton)
        layout.addView(streamButton)
        layout.addView(detailView)

        val scroll = ScrollView(this).apply { addView(layout) }
        setContentView(scroll)

        ContextCompat.registerReceiver(
            this,
            usbReceiver,
            IntentFilter(ACTION_USB_PERMISSION),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )

        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIFICATIONS)
        }
    }

    override fun onResume() {
        super.onResume()
        updateDeviceInfo()
        autoConnectIfPossible()
        handler.post(poll)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        updateDeviceInfo()
        autoConnectIfPossible()
    }

    override fun onPause() {
        handler.removeCallbacks(poll)
        super.onPause()
    }

    override fun onDestroy() {
        destroyed = true
        pendingDevice = null
        handler.removeCallbacks(poll)
        try {
            unregisterReceiver(usbReceiver)
        } catch (_: Exception) {
        }
        super.onDestroy()
    }

    private fun autoConnectIfPossible() {
        val status = DongleService.status
        if (status != "idle" && status != "stopped") return
        val usb = getSystemService(UsbManager::class.java)
        val driver = UsbSerialProber.getDefaultProber()
            .findAllDrivers(usb)
            .firstOrNull() ?: return
        if (usb.hasPermission(driver.device)) {
            startDongle(driver.device)
        } else {
            requestUsbPermission(usb, driver.device)
        }
    }

    private fun connectDongle() {
        val usb = getSystemService(UsbManager::class.java)
        val driver = UsbSerialProber.getDefaultProber()
            .findAllDrivers(usb)
            .firstOrNull()

        if (driver == null) {
            statusView.text = getString(R.string.no_device)
            updateDeviceInfo()
            return
        }

        val device = driver.device
        if (!usb.hasPermission(device)) {
            requestUsbPermission(usb, device)
            statusView.text = getString(R.string.waiting_permission)
            return
        }

        startDongle(device)
    }

    private fun requestUsbPermission(usb: UsbManager, device: UsbDevice) {
        pendingDevice = device
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    PendingIntent.FLAG_MUTABLE
                } else {
                    0
                }
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        val pending = PendingIntent.getBroadcast(this, 0, intent, flags)
        usb.requestPermission(device, pending)
        detailView.text = "Device: ${device.deviceName}\nVID:PID ${device.vendorId}:${device.productId}"
    }

    private fun startDongle(device: UsbDevice) {
        pendingDevice = null
        val intent = Intent(this, DongleService::class.java)
            .putExtra(DongleService.EXTRA_DEVICE_NAME, device.deviceName)
        ContextCompat.startForegroundService(this, intent)
        statusView.text = "starting..."
    }

    private fun stopDongle() {
        stopService(Intent(this, DongleService::class.java))
        statusView.text = "stopped"
    }

    private fun applyConfig() {
        configFieldsDirty = false
        configView.text = getString(R.string.config_sending)

        if (manualUidView.isChecked) {
            val uid = DongleConfig.parseUid(uidView.text.toString())
            if (uid == null) {
                configView.text = getString(R.string.bad_uid)
                return
            }
            val normalized = DongleConfig.normalizeUid(uid)
            if (DongleConfig.uidIsZero(normalized)) {
                configView.text = getString(R.string.uid_required)
                return
            }
            startService(
                Intent(this, DongleService::class.java)
                    .setAction(DongleService.ACTION_APPLY_UID)
                    .putExtra(DongleService.EXTRA_UID, normalized)
                    .putExtra(DongleService.EXTRA_CHANNEL, FIXED_CHANNEL)
            )
            return
        }

        // Empty phrase is valid: the dongle falls back to its factory MAC.
        val phrase = phraseView.text.toString()
        startService(
            Intent(this, DongleService::class.java)
                .setAction(DongleService.ACTION_APPLY_CONFIG)
                .putExtra(DongleService.EXTRA_PHRASE, phrase)
                .putExtra(DongleService.EXTRA_CHANNEL, FIXED_CHANNEL)
        )
    }

    private fun sendConfigAction(action: String) {
        forceConfigFill = true
        configFieldsDirty = false
        startService(Intent(this, DongleService::class.java).setAction(action))
    }

    private fun applyConfigFromDongle() {
        val info = DongleService.configInfo ?: return
        val revision = DongleService.configRevision
        if (revision == lastConfigRevision) return
        if (!forceConfigFill && configFieldsDirty) return
        loadingConfigFields = true
        phraseView.setText(info.phrase)
        manualUidView.isChecked = info.uidMode == DongleConfig.UID_MODE_MANUAL
        uidView.setText(info.uid)
        loadingConfigFields = false
        updateModeFields()
        updateApplyEnabled()
        configFieldsDirty = false
        forceConfigFill = false
        lastConfigRevision = revision
    }

    private fun isStreaming(): Boolean {
        val s = DongleService.status
        return s != "idle" && s != "stopped" &&
                !s.startsWith("no device") && !s.startsWith("USB permission")
    }

    private fun updateModeFields() {
        phraseView.isEnabled = !manualUidView.isChecked
        uidView.isEnabled = manualUidView.isChecked
        if (::applyConfigButton.isInitialized) updateApplyEnabled()
    }

    private fun updateApplyEnabled() {
        if (!::applyConfigButton.isInitialized) return
        applyConfigButton.isEnabled = if (manualUidView.isChecked) {
            val uid = DongleConfig.parseUid(uidView.text.toString())
            uid != null && !DongleConfig.uidIsZero(DongleConfig.normalizeUid(uid))
        } else {
            true
        }
    }

    private data class Target(val host: String, val port: Int)

    private fun parseTarget(raw: String): Target? {
        val s = raw.trim()
        if (s.isEmpty()) return Target("", NetOut.DEFAULT_PORT)

        var host: String
        var port = NetOut.DEFAULT_PORT
        var ipv6 = false
        if (s.startsWith("[")) {
            val end = s.indexOf(']')
            if (end < 0) return null
            host = s.substring(1, end)
            ipv6 = true
            val rest = s.substring(end + 1)
            if (rest.isNotEmpty()) {
                if (!rest.startsWith(":")) return null
                val portText = rest.substring(1)
                if (portText.isEmpty()) return null
                port = portText.toIntOrNull() ?: return null
            }
        } else {
            val first = s.indexOf(':')
            val last = s.lastIndexOf(':')
            if (first >= 0 && first != last) return null
            if (first >= 0) {
                host = s.substring(0, first)
                val portText = s.substring(first + 1)
                if (portText.isEmpty()) return null
                port = portText.toIntOrNull() ?: return null
            } else {
                host = s
            }
        }

        if (host.isBlank() || port !in 1..65535) return null
        val hostOk = if (ipv6) {
            host.matches(Regex("^[0-9A-Fa-f:]+$"))
        } else {
            host.matches(Regex("^[A-Za-z0-9._-]+$"))
        }
        if (!hostOk) return null
        return Target(host, port)
    }

    private fun applyTarget() {
        val target = parseTarget(targetView.text.toString())
        if (target == null) {
            targetView.error = getString(R.string.bad_target)
            targetView.setTextColor(Color.RED)
            configView.text = getString(R.string.bad_target)
            return
        }
        targetView.error = null
        targetView.setTextColor(Color.BLACK)
        val text = if (target.host.isEmpty()) "" else "${target.host}:${target.port}"
        targetView.setText(text)

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(PREF_TARGET_HOST, target.host)
            .putInt(PREF_TARGET_PORT, target.port)
            .apply()
        startService(
            Intent(this, DongleService::class.java)
                .setAction(DongleService.ACTION_UPDATE_TARGET)
        )
        configView.text = if (target.host.isEmpty()) {
            getString(R.string.target_broadcast)
        } else {
            getString(R.string.target_saved, target.host, target.port)
        }
    }

    private fun updateDeviceInfo() {
        val usb = getSystemService(UsbManager::class.java)
        val all = usb.deviceList.values
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usb)
        val supported = drivers.joinToString("\n") {
            "${it.javaClass.simpleName}: ${it.device.deviceName} " +
                    "VID:PID ${it.device.vendorId}:${it.device.productId}"
        }
        detailView.text = buildString {
            append("USB devices: ${all.size}\n")
            if (supported.isNotEmpty()) {
                append("Supported:\n").append(supported)
            } else {
                append("Supported serial devices: none")
            }
        }
        detailView.visibility = View.VISIBLE
    }
}
