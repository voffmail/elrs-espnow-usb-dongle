package com.elrs.dongle.stub

import java.io.ByteArrayOutputStream

/**
 * Runtime configuration protocol shared with the dongle and tools/dongle_config.py.
 *
 * Frame: AA 55 | ver=1 | type | len LE16 | payload | CRC16 LE16
 * CRC16-CCITT-FALSE over ver..payload.
 */
object DongleConfig {

    const val MAGIC0 = 0xAA
    const val MAGIC1 = 0x55
    const val VERSION = 1
    const val MAX_PAYLOAD = 128
    const val MAX_PHRASE = 64

    const val GET_CONFIG = 0x01
    const val SET_CONFIG = 0x02
    const val GET_STATS = 0x03
    const val RESET = 0x04
    const val PING = 0x05
    const val SET_UID = 0x06
    const val RESPONSE = 0x80

    const val UID_MODE_PHRASE = 0
    const val UID_MODE_MANUAL = 1

    const val OK = 0
    const val ERR_BAD_REQUEST = 1
    const val ERR_TOO_LONG = 2
    const val ERR_INVALID = 3
    const val ERR_BUSY = 4

    data class Frame(val type: Int, val payload: ByteArray)

    data class ConfigInfo(
        val channel: Int,
        val uid: String,
        val phrase: String,
        val version: String,
        val uidMode: Int,
    )

    fun statusName(status: Int): String = when (status) {
        OK -> "ok"
        ERR_BAD_REQUEST -> "bad request"
        ERR_TOO_LONG -> "too long"
        ERR_INVALID -> "invalid"
        ERR_BUSY -> "busy"
        else -> "0x" + status.toString(16)
    }

    fun crc16(data: ByteArray, offset: Int = 0, length: Int = data.size): Int {
        var crc = 0xFFFF
        for (i in offset until offset + length) {
            crc = crc xor ((data[i].toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if ((crc and 0x8000) != 0) {
                    ((crc shl 1) xor 0x1021) and 0xFFFF
                } else {
                    (crc shl 1) and 0xFFFF
                }
            }
        }
        return crc
    }

    fun build(msgType: Int, payload: ByteArray = ByteArray(0)): ByteArray {
        require(payload.size <= MAX_PAYLOAD)
        val out = ByteArrayOutputStream(6 + payload.size + 2)
        out.write(MAGIC0)
        out.write(MAGIC1)
        out.write(VERSION)
        out.write(msgType)
        out.write(payload.size and 0xFF)
        out.write((payload.size ushr 8) and 0xFF)
        out.write(payload)
        val body = out.toByteArray()
        val crc = crc16(body, 2, 4 + payload.size)
        out.write(crc and 0xFF)
        out.write((crc ushr 8) and 0xFF)
        return out.toByteArray()
    }

    fun buildGetConfig(): ByteArray = build(GET_CONFIG)

    fun buildSetUid(uid: ByteArray, channel: Int): ByteArray {
        require(uid.size == 6)
        val normalized = normalizeUid(uid)
        require(!uidIsZero(normalized))
        val payload = ByteArray(7)
        payload[0] = channel.toByte()
        normalized.copyInto(payload, 1)
        return build(SET_UID, payload)
    }

    fun buildSetConfig(phrase: String, channel: Int): ByteArray {
        val phraseBytes = phrase.toByteArray(Charsets.UTF_8)
        require(phraseBytes.size <= MAX_PHRASE)
        val payload = ByteArray(2 + phraseBytes.size)
        payload[0] = channel.toByte()
        payload[1] = phraseBytes.size.toByte()
        phraseBytes.copyInto(payload, 2)
        return build(SET_CONFIG, payload)
    }

    fun buildGetStats(): ByteArray = build(GET_STATS)

    fun buildPing(): ByteArray = build(PING)

    fun buildReset(): ByteArray = build(RESET)

    fun parseConfig(payload: ByteArray): ConfigInfo? {
        if (payload.size < 9) return null
        if (payload[0].toInt() != OK) return null
        val channel = payload[1].toInt() and 0xFF
        val uid = uidString(payload, 2)
        val phraseLen = payload[8].toInt() and 0xFF
        val end = minOf(9 + phraseLen, payload.size)
        val phrase = String(payload, 9, end - 9, Charsets.UTF_8)
        val version = if (payload.size >= end + 3) {
            "${payload[end].toInt() and 0xFF}.${payload[end + 1].toInt() and 0xFF}.${payload[end + 2].toInt() and 0xFF}"
        } else {
            "?"
        }
        val uidMode = if (payload.size > end + 3) {
            payload[end + 3].toInt() and 0xFF
        } else {
            UID_MODE_PHRASE
        }
        return ConfigInfo(channel, uid, phrase, version, uidMode)
    }

    fun decodeConfig(payload: ByteArray): String {
        if (payload.size < 9) return "short config response"
        if (payload[0].toInt() != OK) return "device: " + statusName(payload[0].toInt())
        val info = parseConfig(payload) ?: return "invalid config response"
        val mode = if (info.uidMode == UID_MODE_MANUAL) "manual UID" else "phrase"
        return "mode=$mode channel=${info.channel} uid=${info.uid} " +
                "phrase=${info.phrase.ifEmpty { "<empty>" }} ver=${info.version}"
    }

    fun parseUid(text: String): ByteArray? {
        val cleaned = text.trim().replace(":", "").replace("-", "").replace(" ", "")
        if (cleaned.length != 12 || cleaned.any { it !in "0123456789abcdefABCDEF" }) {
            return null
        }
        return ByteArray(6) { i ->
            cleaned.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
    }

    fun normalizeUid(uid: ByteArray): ByteArray {
        require(uid.size == 6)
        val out = uid.copyOf()
        out[0] = (out[0].toInt() and 0xFE).toByte()
        return out
    }

    fun uidIsZero(uid: ByteArray): Boolean = uid.all { it.toInt() == 0 }

    fun decodeAck(payload: ByteArray): String {
        if (payload.isEmpty()) return "empty response"
        val status = payload[0].toInt() and 0xFF
        if (status != OK || payload.size < 8) return statusName(status)
        return "ok, channel=${payload[1].toInt() and 0xFF}, uid=${uidString(payload, 2)}"
    }

    fun decodeStats(payload: ByteArray): String {
        if (payload.size < 20) return "short stats response"
        if (payload[0].toInt() != OK) return statusName(payload[0].toInt())
        val packets = readU32(payload, 1)
        val bytes = readU32(payload, 5)
        val drops = readU32(payload, 9)
        val lastLen = (payload[13].toInt() and 0xFF) or ((payload[14].toInt() and 0xFF) shl 8)
        val lastMs = readU32(payload, 15)
        val ready = payload[19].toInt() and 0xFF
        return "ready=$ready packets=$packets bytes=$bytes drops=$drops last=$lastLen/${lastMs}ms"
    }

    private fun uidString(data: ByteArray, offset: Int): String {
        if (offset + 6 > data.size) return "?"
        return (0 until 6).joinToString(":") {
            "%02X".format(data[offset + it].toInt() and 0xFF)
        }
    }

    private fun readU32(data: ByteArray, offset: Int): Long {
        return (data[offset].toLong() and 0xFF) or
                ((data[offset + 1].toLong() and 0xFF) shl 8) or
                ((data[offset + 2].toLong() and 0xFF) shl 16) or
                ((data[offset + 3].toLong() and 0xFF) shl 24)
    }

    class Parser {
        private val buf = ByteArray(512)
        private var length = 0

        fun reset() {
            length = 0
        }

        fun feed(data: ByteArray, count: Int): List<Frame> {
            if (count <= 0) return emptyList()
            if (length + count > buf.size) {
                val keep = minOf(length, 256)
                System.arraycopy(buf, length - keep, buf, 0, keep)
                length = keep
            }
            System.arraycopy(data, 0, buf, length, count)
            length += count

            val out = ArrayList<Frame>()
            var i = 0
            while (i + 1 < length) {
                if ((buf[i].toInt() and 0xFF) != MAGIC0 ||
                    (buf[i + 1].toInt() and 0xFF) != MAGIC1
                ) {
                    i++
                    continue
                }
                if (i + 6 > length) break
                if ((buf[i + 2].toInt() and 0xFF) != VERSION) {
                    i++
                    continue
                }
                val size = (buf[i + 4].toInt() and 0xFF) or
                        ((buf[i + 5].toInt() and 0xFF) shl 8)
                if (size > MAX_PAYLOAD) {
                    i++
                    continue
                }
                val total = 6 + size + 2
                if (i + total > length) break
                val expected = (buf[i + 6 + size].toInt() and 0xFF) or
                        ((buf[i + 7 + size].toInt() and 0xFF) shl 8)
                val actual = crc16(buf, i + 2, 4 + size)
                if (expected != actual) {
                    i++
                    continue
                }
                val type = buf[i + 3].toInt() and 0xFF
                val payload = buf.copyOfRange(i + 6, i + 6 + size)
                out.add(Frame(type, payload))
                i += total
            }
            if (i > 0) {
                System.arraycopy(buf, i, buf, 0, length - i)
                length -= i
            }
            return out
        }
    }
}
