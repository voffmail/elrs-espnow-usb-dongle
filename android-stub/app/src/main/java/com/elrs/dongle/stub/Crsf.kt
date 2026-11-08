package com.elrs.dongle.stub

import android.os.SystemClock

/**
 * Extracts complete CRSF frames from the USB serial byte stream.
 *
 * The dongle forwards raw ESP-NOW payloads, which contain the CRSF frame after
 * an `$X<` wrapper. We scan for CRSF address bytes and validate CRC8, so the
 * wrapper and boot ROM noise are ignored.
 */
class CrsfFramer(private val onFrame: (ByteArray) -> Unit) {

    private val buf = ByteArray(8192)
    private var length = 0

    @Volatile var frames = 0L
        private set

    @Volatile var crcErrors = 0L
        private set

    @Volatile var bytes = 0L
        private set

    @Volatile var lastType = -1
        private set

    @Volatile var lastLen = 0
        private set

    @Volatile var lastAt = 0L
        private set

    @Synchronized
    fun feed(data: ByteArray, count: Int) {
        if (count <= 0) return
        bytes += count

        if (length + count > buf.size) {
            val keep = minOf(length, 256)
            System.arraycopy(buf, length - keep, buf, 0, keep)
            length = keep
        }
        System.arraycopy(data, 0, buf, length, count)
        length += count

        var i = 0
        while (i + 1 < length) {
            val address = buf[i].toInt() and 0xFF
            if (address == 0xC8 || address == 0xEA || address == 0xEE) {
                val frameLength = buf[i + 1].toInt() and 0xFF
                if (frameLength in 2..62) {
                    val total = frameLength + 2
                    if (i + total > length) {
                        // Incomplete frame at the end: keep it for the next read.
                        break
                    }
                    val crcIndex = i + total - 1
                    if (crc8(buf, i + 2, crcIndex) == (buf[crcIndex].toInt() and 0xFF)) {
                        val frame = buf.copyOfRange(i, i + total)
                        lastType = frame[2].toInt() and 0xFF
                        lastLen = frameLength
                        lastAt = SystemClock.elapsedRealtime()
                        frames++
                        onFrame(frame)
                        i += total
                        continue
                    } else {
                        crcErrors++
                    }
                }
            }
            i++
        }

        if (i > 0) {
            System.arraycopy(buf, i, buf, 0, length - i)
            length -= i
        }
    }

    private fun crc8(data: ByteArray, from: Int, until: Int): Int {
        var crc = 0
        for (i in from until until) {
            crc = crc xor (data[i].toInt() and 0xFF)
            repeat(8) {
                crc = if ((crc and 0x80) != 0) {
                    ((crc shl 1) xor 0xD5) and 0xFF
                } else {
                    (crc shl 1) and 0xFF
                }
            }
        }
        return crc
    }
}
