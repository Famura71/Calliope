package com.example.calliope_mobile

import android.content.Context
import android.util.Log
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.example.calliope_mobile.ui.theme.Calliope_MobileTheme
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

private const val MAGIC = 0x31504C43
private const val SYNC_MAGIC = 0x31434E53
private const val PORT = 4010
private const val CONNECT_TIMEOUT_MS = 600
private const val SYNC_PROBE_COUNT = 1
private const val LATENCY_LOG_TAG = "CalliopeLatency"
private const val MAX_ACCEPTABLE_LATENCY_US = 35_000L
private const val STARTUP_DROP_LATENCY_US = 60_000L

class MainActivity : ComponentActivity() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var streamJob: Job? = null
    private val latencyMeasurementEnabled = AtomicBoolean(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            Calliope_MobileTheme {
                var host by remember { mutableStateOf("") }
                var status by remember { mutableStateOf("Hazir") }
                var connected by remember { mutableStateOf(false) }
                var latencyEnabled by remember { mutableStateOf(false) }

                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Text("Calliope Mobile Receiver", style = MaterialTheme.typography.titleMedium)
                    OutlinedTextField(
                        value = host,
                        onValueChange = { host = it },
                        modifier = Modifier.fillMaxWidth(),
                        label = { Text("PC IP") },
                        placeholder = { Text("Ornek: 192.168.1.25") },
                        singleLine = true
                    )
                    Text("Port: $PORT")
                    Text("Durum: $status")
                    Text("Latency olcumu: ${if (latencyEnabled) "Acik" else "Kapali"}")
                    Button(
                        onClick = {
                            if (connected) return@Button
                            status = "IP aranıyor..."
                            streamJob = scope.launch {
                                val found = scanForServerHost()
                                runOnUiThread {
                                    if (found != null) {
                                        host = found
                                        status = "IP bulundu: $found"
                                    } else {
                                        status = "Aynı ağda Calliope bulunamadı"
                                    }
                                }
                            }
                        }
                    ) {
                        Text("IP Otomatik Bul")
                    }
                    Button(
                        onClick = {
                            latencyEnabled = !latencyEnabled
                            latencyMeasurementEnabled.set(latencyEnabled)
                            Log.d(
                                LATENCY_LOG_TAG,
                                if (latencyEnabled) "Latency measurement enabled by user"
                                else "Latency measurement disabled by user"
                            )
                        }
                    ) {
                        Text(if (latencyEnabled) "Latency Olcumunu Durdur" else "Latency Olcumunu Baslat")
                    }
                    Button(
                        onClick = {
                            if (!connected) {
                                if (host.isBlank()) {
                                    status = "Lutfen PC IP gir"
                                    return@Button
                                }
                                connected = true
                                status = "Baglaniyor..."
                                streamJob = scope.launch {
                                    runCatching { streamAudio(host) }
                                        .onSuccess {
                                            runOnUiThread {
                                                status = "Baglanti kapandi"
                                                connected = false
                                            }
                                        }
                                        .onFailure {
                                            runOnUiThread {
                                                status = "Hata: ${it.message}"
                                                connected = false
                                            }
                                        }
                                }
                            } else {
                                connected = false
                                status = "Durduruldu"
                                streamJob?.cancel()
                            }
                        }
                    ) {
                        Text(if (connected) "Durdur" else "Baglan")
                    }
                }
            }
        }
    }

    private fun scanForServerHost(): String? {
        val local = resolveLocalIpv4Address() ?: return null
        val parts = local.split(".")
        if (parts.size != 4) return null
        val subnet = "${parts[0]}.${parts[1]}.${parts[2]}"

        val pool = Executors.newFixedThreadPool(48)
        try {
            val futures = (1..254).map { i ->
                val host = "$subnet.$i"
                pool.submit<String?> {
                    try {
                        Socket().use { s ->
                            s.connect(InetSocketAddress(host, PORT), CONNECT_TIMEOUT_MS)
                        }
                        host
                    } catch (_: Exception) {
                        null
                    }
                }
            }
            for (future in futures) {
                val result = runCatching { future.get(250, TimeUnit.MILLISECONDS) }.getOrNull()
                if (result != null) {
                    return result
                }
            }
        } finally {
            pool.shutdownNow()
        }
        return null
    }

    private fun resolveLocalIpv4Address(): String? {
        val wifiAddress = runCatching {
            val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            val ip = wifiManager?.connectionInfo?.ipAddress ?: 0
            if (ip == 0) {
                null
            } else {
                "%d.%d.%d.%d".format(
                    ip and 0xff,
                    ip shr 8 and 0xff,
                    ip shr 16 and 0xff,
                    ip shr 24 and 0xff
                )
            }
        }.getOrNull()
        if (!wifiAddress.isNullOrBlank() && wifiAddress != "0.0.0.0") {
            return wifiAddress
        }

        return NetworkInterface.getNetworkInterfaces()
            ?.toList()
            ?.asSequence()
            ?.filter { it.isUp && !it.isLoopback }
            ?.flatMap { it.inetAddresses.toList().asSequence() }
            ?.filterIsInstance<Inet4Address>()
            ?.mapNotNull { it.hostAddress }
            ?.firstOrNull { !it.startsWith("127.") }
    }

    private fun streamAudio(host: String) {
        Socket().use { socket ->
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(host, PORT), CONNECT_TIMEOUT_MS)

            val input = DataInputStream(socket.getInputStream())
            val output = DataOutputStream(socket.getOutputStream())
            val clockOffsetUs = performClockSync(input, output)
            var audioTrack: AudioTrack? = null
            var frameSizeBytes = 0
            var packetsSinceReport = 0
            var latencySumUs = 0L
            var latencyMaxUs = Long.MIN_VALUE
            var latencyMinUs = Long.MAX_VALUE
            var lastSequence = -1
            var droppedPackets = 0
            try {
                while (scope.isActive) {
                    val headerBytes = ByteArray(30)
                    input.readFully(headerBytes)
                    val header = ByteBuffer.wrap(headerBytes).order(ByteOrder.LITTLE_ENDIAN)

                    val magic = header.int
                    if (magic != MAGIC) {
                        throw IllegalStateException("Invalid stream header")
                    }
                    val sequence = header.int
                    val captureTimeUs = header.long
                    val sampleRate = header.int
                    val channels = header.short.toInt()
                    val bits = header.short.toInt()
                    val formatTag = header.short.toInt()
                    val payloadBytes = header.int

                    val payload = ByteArray(payloadBytes)
                    input.readFully(payload)

                    val receiveTimeUs = nowUs()
                    val estimatedCaptureOnMobileClockUs = captureTimeUs - clockOffsetUs
                    val latencyUs = receiveTimeUs - estimatedCaptureOnMobileClockUs

                    if (audioTrack == null) {
                        val channelMask = if (channels == 1) {
                            AudioFormat.CHANNEL_OUT_MONO
                        } else {
                            AudioFormat.CHANNEL_OUT_STEREO
                        }
                        val encoding = when {
                            formatTag == 3 && bits == 32 -> AudioFormat.ENCODING_PCM_FLOAT
                            bits == 16 -> AudioFormat.ENCODING_PCM_16BIT
                            else -> AudioFormat.ENCODING_PCM_16BIT
                        }
                        frameSizeBytes = channels * (bits / 8)
                        val minBuffer = AudioTrack.getMinBufferSize(sampleRate, channelMask, encoding)
                            .coerceAtLeast(payloadBytes)

                        audioTrack = AudioTrack.Builder()
                            .setAudioAttributes(
                                AudioAttributes.Builder()
                                    .setUsage(AudioAttributes.USAGE_MEDIA)
                                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                    .build()
                            )
                            .setAudioFormat(
                                AudioFormat.Builder()
                                    .setSampleRate(sampleRate)
                                    .setEncoding(encoding)
                                    .setChannelMask(channelMask)
                                    .build()
                            )
                            .setTransferMode(AudioTrack.MODE_STREAM)
                            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                            .setBufferSizeInBytes(minBuffer)
                            .build()
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            val startThresholdFrames = (payloadBytes / frameSizeBytes).coerceAtLeast(1)
                            audioTrack.setStartThresholdInFrames(startThresholdFrames)
                        }
                        audioTrack.play()
                    }

                    if (lastSequence >= 0 && sequence != lastSequence + 1) {
                        Log.w(LATENCY_LOG_TAG, "Sequence jump: expected=${lastSequence + 1}, actual=$sequence")
                    }
                    lastSequence = sequence

                    if (latencyUs > STARTUP_DROP_LATENCY_US || (droppedPackets > 0 && latencyUs > MAX_ACCEPTABLE_LATENCY_US)) {
                        droppedPackets += 1
                        if (droppedPackets % 10 == 1) {
                            Log.w(
                                LATENCY_LOG_TAG,
                                "Dropping stale packet seq=$sequence latencyMs=${latencyUs / 1000.0} drops=$droppedPackets"
                            )
                        }
                        continue
                    }

                    if (droppedPackets > 0) {
                        Log.d(LATENCY_LOG_TAG, "Recovered after dropping $droppedPackets stale packets")
                        droppedPackets = 0
                    }

                    audioTrack.write(payload, 0, payload.size, AudioTrack.WRITE_NON_BLOCKING)

                    if (latencyMeasurementEnabled.get()) {
                        packetsSinceReport += 1
                        latencySumUs += latencyUs
                        latencyMaxUs = maxOf(latencyMaxUs, latencyUs)
                        latencyMinUs = minOf(latencyMinUs, latencyUs)

                        if (packetsSinceReport >= 50) {
                            val avgMs = latencySumUs / packetsSinceReport / 1000.0
                            val minMs = latencyMinUs / 1000.0
                            val maxMs = latencyMaxUs / 1000.0
                            val message = "Latency avg=%.1f ms min=%.1f ms max=%.1f ms".format(avgMs, minMs, maxMs)
                            Log.d(LATENCY_LOG_TAG, message)
                            packetsSinceReport = 0
                            latencySumUs = 0L
                            latencyMaxUs = Long.MIN_VALUE
                            latencyMinUs = Long.MAX_VALUE
                        }
                    } else if (packetsSinceReport != 0) {
                        packetsSinceReport = 0
                        latencySumUs = 0L
                        latencyMaxUs = Long.MIN_VALUE
                        latencyMinUs = Long.MAX_VALUE
                    }
                }
            } finally {
                audioTrack?.stop()
                audioTrack?.release()
            }
        }
    }

    private fun performClockSync(input: DataInputStream, output: DataOutputStream): Long {
        var bestOffsetUs = 0L
        var bestRttUs = Long.MAX_VALUE

        repeat(SYNC_PROBE_COUNT) {
            val clientSendUs = nowUs()
            val request = ByteBuffer.allocate(12)
                .order(ByteOrder.LITTLE_ENDIAN)
                .putInt(SYNC_MAGIC)
                .putLong(clientSendUs)
                .array()
            output.write(request)
            output.flush()

            val replyBytes = ByteArray(28)
            input.readFully(replyBytes)
            val reply = ByteBuffer.wrap(replyBytes).order(ByteOrder.LITTLE_ENDIAN)
            val replyMagic = reply.int
            val echoedClientSendUs = reply.long
            val serverReceiveUs = reply.long
            val serverSendUs = reply.long
            val clientReceiveUs = nowUs()

            if (replyMagic != SYNC_MAGIC || echoedClientSendUs != clientSendUs) {
                throw IllegalStateException("Invalid sync reply")
            }

            val rttUs = clientReceiveUs - clientSendUs - (serverSendUs - serverReceiveUs)
            val offsetUs = ((serverReceiveUs - clientSendUs) + (serverSendUs - clientReceiveUs)) / 2

            if (rttUs < bestRttUs) {
                bestRttUs = rttUs
                bestOffsetUs = offsetUs
            }
        }

        Log.d(LATENCY_LOG_TAG, "Clock sync done: bestRtt=${bestRttUs / 1000.0} ms offsetUs=$bestOffsetUs")
        return bestOffsetUs
    }

    private fun nowUs(): Long {
        return System.nanoTime() / 1_000L
    }

    override fun onDestroy() {
        super.onDestroy()
        streamJob?.cancel()
        scope.cancel()
    }
}
