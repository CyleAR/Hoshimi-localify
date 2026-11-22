package io.github.cylear.hoshimi.localify

import android.app.Activity
import android.app.AlertDialog
import android.app.ProgressDialog
import android.graphics.Color
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.Log
import android.util.TypedValue
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLConnection
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * The in-game server picker: a plain [AlertDialog] built in code, so it works without the companion
 * module app and without any layout resources in the patched APK.
 *
 * Adding a private server bootstraps its CA from `<url>/.well-known/sumire/ca.pem`:
 *  - an HTTPS instance behind a certificate the system already trusts is accepted as is;
 *  - an HTTPS instance with a self-signed certificate is fetched once with a trust-all connection
 *    (bootstrap only), the returned CA is checked against the presented leaf certificate, and the
 *    fingerprint is confirmed by the player unless `#sha256=` matched;
 *  - plain HTTP always requires the fingerprint confirmation.
 *
 * Network work never runs on the UI thread.
 */
object ServerPromptDialog {
    private const val CA_PATH = "/.well-known/sumire/ca.pem"
    private const val TIMEOUT_MS = 10_000
    private const val ADD_PROFILE_LABEL = "Add private server…"
    private const val OFFICIAL_LABEL = "Official server"

    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var showing = false
    private var selectionChanged: (() -> Unit)? = null

    /**
     * First-launch prompt: only while nothing has ever been selected. A server chosen through the
     * module app has already been migrated to `active.json` by then and is not asked again.
     */
    fun maybeShowFirstLaunch(activity: Activity) {
        try {
            if (activity.isFinishing || activity.isDestroyed) return
            if (ServerProfileStore.appManagedPrivateServer) return
            if (ServerProfileStore.readActiveId(activity.filesDir) != null) return
            show(activity)
        }
        catch (t: Throwable) {
            Log.w(TAG, "ServerSwitch: first-launch prompt failed", t)
        }
    }

    /** Opens the picker. Also used by the "Switch server" overlay button. */
    fun show(activity: Activity, onSelected: (() -> Unit)? = null) {
        selectionChanged = onSelected
        if (activity.isFinishing || activity.isDestroyed) {
            Log.i(TAG, "ServerSwitch: ignoring the server dialog for a finished activity")
            return
        }
        if (showing) {
            Log.i(TAG, "ServerSwitch: the server dialog is already open")
            return
        }
        showing = true
        try {
            showListDialog(activity)
        }
        catch (t: Throwable) {
            showing = false
            Log.e(TAG, "ServerSwitch: cannot build the server dialog", t)
            Toast.makeText(activity, "Cannot open the server dialog: ${t.message}", Toast.LENGTH_LONG).show()
        }
    }

    // ---- profile list --------------------------------------------------------------------

    private fun showListDialog(activity: Activity) {
        val filesDir = activity.filesDir
        val profiles = ServerProfileStore.loadProfiles(filesDir)
        val activeId = ServerProfileStore.readActiveId(filesDir)

        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(activity, 20), dp(activity, 8), dp(activity, 20), dp(activity, 8))
        }
        val group = RadioGroup(activity).apply { orientation = RadioGroup.VERTICAL }
        container.addView(group, matchWidth())

        val dialog = AlertDialog.Builder(activity)
            .setTitle("Select server")
            .setView(ScrollView(activity).apply { addView(container) })
            .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
            .create()
        dialog.setOnDismissListener { showing = false }

        val profileIds = HashMap<Int, String>()
        for (profile in profiles) {
            val radio = RadioButton(activity).apply {
                id = View.generateViewId()
                text = if (profile.kind == ServerProfile.KIND_OFFICIAL) {
                    OFFICIAL_LABEL
                }
                else {
                    profile.label.ifBlank { profile.url }
                }
                textSize = 16f
                setPadding(0, dp(activity, 6), 0, dp(activity, 6))
                if (profile.id == activeId) isChecked = true
            }
            if (profile.kind != ServerProfile.KIND_OFFICIAL && profile.id != activeId) {
                // RadioButton rows must stay direct children of the RadioGroup, so the least
                // invasive place for the extra action is a long-press on the row.
                radio.isLongClickable = true
                radio.setOnLongClickListener {
                    confirmRemove(activity, dialog, profile)
                    true
                }
            }
            profileIds[radio.id] = profile.id
            group.addView(radio, matchWidth())
        }

        val addRadio = RadioButton(activity).apply {
            id = View.generateViewId()
            text = ADD_PROFILE_LABEL
            textSize = 16f
            setPadding(0, dp(activity, 10), 0, dp(activity, 6))
        }
        group.addView(addRadio, matchWidth())

        if (profiles.any { it.kind != ServerProfile.KIND_OFFICIAL && it.id != activeId }) {
            container.addView(TextView(activity).apply {
                text = "Long-press a saved server to remove it."
                textSize = 13f
                alpha = 0.7f
                setPadding(0, dp(activity, 8), 0, 0)
            }, matchWidth())
        }

        group.setOnCheckedChangeListener { _, checkedId ->
            try {
                if (checkedId == addRadio.id) {
                    promptForUrl(activity, dialog, initial = if (activity is MainActivity) activity.programConfig.privateServerUrl else activity.intent.getStringExtra("privateServerDraft").orEmpty())
                    return@setOnCheckedChangeListener
                }
                val profileId = profileIds[checkedId] ?: return@setOnCheckedChangeListener
                handleSelection(activity, dialog, profileId)
            }
            catch (t: Throwable) {
                Log.w(TAG, "ServerSwitch: selection failed", t)
            }
        }
        dialog.show()
    }

    /** Removes a saved but inactive private profile after an explicit confirmation. */
    private fun confirmRemove(activity: Activity, listDialog: AlertDialog, profile: ServerProfile) {
        if (activity.isFinishing || activity.isDestroyed) {
            Log.i(TAG, "ServerSwitch: the activity is gone; skipping the remove confirmation")
            return
        }
        try {
            AlertDialog.Builder(activity)
                .setTitle("Remove server?")
                .setMessage(profile.url)
                .setPositiveButton("Remove") { d, _ ->
                    d.dismiss()
                    val removed = ServerProfileStore.removeProfile(activity.filesDir, profile.id)
                    if (removed) {
                        Toast.makeText(
                            activity,
                            "Removed ${profile.label.ifBlank { profile.url }}",
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                    restartList(activity, listDialog)
                }
                .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
                .show()
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: cannot show the remove confirmation", t)
        }
    }

    private fun handleSelection(activity: Activity, dialog: AlertDialog, profileId: String) {
        val filesDir = activity.filesDir
        try {
            if (activity.packageName == BuildConfig.APPLICATION_ID) {
                ServerProfileStore.writeActiveId(filesDir, profileId)
                dialog.dismiss()
                selectionChanged?.invoke()
                selectionChanged = null
                Toast.makeText(activity, "Server selected. The change applies when you start the game.", Toast.LENGTH_LONG).show()
                return
            }
            val activeId = ServerProfileStore.readActiveId(filesDir)
            if (profileId == activeId) {
                dialog.dismiss()
                return
            }
            if (activeId == null && profileId == ServerProfile.OFFICIAL_ID) {
                // First launch: nothing was ever selected, so there is no account to separate.
                ServerProfileStore.writeActiveId(filesDir, profileId)
                dialog.dismiss()
                return
            }
            // Any other change restarts the process and clears the local account data.
            val displayName = ServerProfileStore.loadProfiles(filesDir)
                .firstOrNull { it.id == profileId }
                ?.let {
                    if (it.kind == ServerProfile.KIND_OFFICIAL) OFFICIAL_LABEL
                    else it.label.ifBlank { it.url }
                }
                ?: profileId
            confirmGameWillClose(activity, dialog, profileId)
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: switch to $profileId failed", t)
            Toast.makeText(activity, "Cannot switch server: ${t.message}", Toast.LENGTH_LONG).show()
        }
    }

    /** Second step: tells the user the game closes to finish the switch and must be reopened. */
    private fun confirmGameWillClose(activity: Activity, listDialog: AlertDialog, profileId: String) {
        if (activity.isFinishing || activity.isDestroyed) {
            Log.i(TAG, "ServerSwitch: the activity is gone; skipping the close notice")
            return
        }
        try {
            AlertDialog.Builder(activity)
                .setTitle("The game will close")
                .setMessage(
                    "The game will close to finish the server change.\n\n" +
                            "Please reopen the game afterwards."
                )
                .setCancelable(false)
                .setPositiveButton("OK") { d, _ ->
                    d.dismiss()
                    listDialog.dismiss()
                    ServerProfileStore.requestSwitch(activity, profileId)
                }
                .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
                .show()
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: cannot show the close notice", t)
        }
    }

    // ---- add a private server ------------------------------------------------------------

    private fun promptForUrl(activity: Activity, listDialog: AlertDialog, initial: String) {
        val input = EditText(activity).apply {
            hint = "https://host:port or host:port"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setSingleLine(true)
            setText(initial)
        }
        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(activity, 20), dp(activity, 8), dp(activity, 20), dp(activity, 8))
        }
        container.addView(input, matchWidth())
        container.addView(TextView(activity).apply {
            text = "Append #sha256=<fingerprint> to pin the certificate and skip the confirmation."
            textSize = 12f
            setTextColor(Color.GRAY)
            setPadding(0, dp(activity, 6), 0, 0)
        }, matchWidth())

        var handled = false
        val dialog = AlertDialog.Builder(activity)
            .setTitle("Add private server")
            .setView(ScrollView(activity).apply { addView(container) })
            .setPositiveButton("Add", null)
            .setNegativeButton("Cancel", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val parsed = ServerProfileStore.parseServerInput(input.text.toString())
                if (parsed == null) {
                    input.error = "Enter host:port or https://host:port (optional #sha256=…)"
                    return@setOnClickListener
                }
                handled = true
                dialog.dismiss()
                startBootstrap(activity, parsed, input.text.toString(), listDialog)
            }
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener { dialog.dismiss() }
        }
        dialog.setOnDismissListener {
            if (!handled) restartList(activity, listDialog)
        }
        dialog.show()
    }

    private fun restartList(activity: Activity, listDialog: AlertDialog) {
        try {
            if (listDialog.isShowing) listDialog.dismiss()
        }
        catch (t: Throwable) {
            Log.w(TAG, "ServerSwitch: cannot dismiss the server list", t)
        }
        showing = false
        mainHandler.post { show(activity) }
    }

    private fun startBootstrap(
        activity: Activity,
        input: ServerProfileStore.ServerInput,
        rawText: String,
        listDialog: AlertDialog
    ) {
        @Suppress("DEPRECATION")
        val progress = ProgressDialog(activity).apply {
            setMessage("Contacting ${input.address.label}…")
            setCancelable(false)
            show()
        }
        Thread {
            val result = try {
                fetchCa(input)
            }
            catch (t: Throwable) {
                Log.w(TAG, "ServerSwitch: CA bootstrap for ${input.address.url} failed", t)
                BootstrapResult(error = readableError(t))
            }
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) {
                    Log.i(TAG, "ServerSwitch: the activity is gone; dropping the CA bootstrap result")
                    return@runOnUiThread
                }
                try {
                    progress.dismiss()
                }
                catch (_: Throwable) {
                }
                try {
                    when {
                        result.error != null -> showBootstrapError(activity, result.error, rawText, listDialog)
                        result.profile == null ->
                            showBootstrapError(
                                activity,
                                "The server did not return a valid CA certificate.",
                                rawText,
                                listDialog
                            )
                        result.needsConfirmation ->
                            confirmFingerprint(activity, result.profile, listDialog)
                        else -> acceptProfile(activity, result.profile, listDialog)
                    }
                }
                catch (t: Throwable) {
                    Log.e(TAG, "ServerSwitch: cannot show the CA bootstrap result", t)
                }
            }
        }.start()
    }

    data class BootstrapResult(
        val profile: ServerProfile? = null,
        val needsConfirmation: Boolean = false,
        val error: String? = null,
    )

    fun fetchCa(input: ServerProfileStore.ServerInput): BootstrapResult {
        val result = fetchCaCertificate(input)
        val profile = result.profile ?: return result
        return try {
            val infoUrl = input.address.url + "/.well-known/sumire/server.json"
            val content = try {
                httpsGet(infoUrl, trustAll = false).first
            } catch (failure: Throwable) {
                if (!isTlsTrustError(failure)) throw failure
                val (body, leaf) = httpsGet(infoUrl, trustAll = true)
                val ca = ServerProfileStore.parseFirstCertificate(profile.caPem)
                    ?: throw IOException("Invalid CA certificate")
                leaf?.checkValidity() ?: throw IOException("Missing server certificate")
                leaf.verify(ca.publicKey)
                body
            }
            val info = org.json.JSONObject(content)
            if (!ServerProfileStore.fingerprintMatches(info.getString("caSha256"), profile.caSha256)) {
                return BootstrapResult(error = "Server information and CA fingerprints do not match.")
            }
            result.copy(profile = profile.copy(label = info.optString("name").take(100).ifBlank { profile.label }))
        } catch (failure: Throwable) {
            BootstrapResult(error = readableError(failure))
        }
    }

    private fun fetchCaCertificate(input: ServerProfileStore.ServerInput): BootstrapResult {
        val address = input.address
        if (address.scheme != "https") {
            return BootstrapResult(error = "An HTTPS server address is required to connect the game.")
        }
        val caUrl = address.url + CA_PATH
        if (address.scheme == "http") {
            val pem = try {
                httpGet(caUrl)
            }
            catch (t: Throwable) {
                return BootstrapResult(error = readableError(t))
            }
            val profile = ServerProfileStore.buildPrivateProfile(address.url, pem)
                ?: return BootstrapResult(error = "The server did not return a valid CA certificate.")
            // Plain HTTP proves nothing about the certificate the game will see later.
            return BootstrapResult(profile = profile, needsConfirmation = true)
        }

        // 1. The certificate chain is already trusted by the system: a public instance on its own
        //    domain. Accept the published CA without asking (unless the player pinned one).
        try {
            val (pem, _) = httpsGet(caUrl, trustAll = false)
            val profile = ServerProfileStore.buildPrivateProfile(address.url, pem)
                ?: return BootstrapResult(error = "The server did not return a valid CA certificate.")
            val pin = input.sha256Pin
            if (pin != null && !ServerProfileStore.fingerprintMatches(pin, profile.caSha256)) {
                return BootstrapResult(error = "The CA fingerprint does not match the #sha256= value in the address.")
            }
            return BootstrapResult(profile = profile)
        }
        catch (t: Throwable) {
            if (!isTlsTrustError(t)) {
                return BootstrapResult(error = readableError(t))
            }
            Log.i(
                TAG,
                "ServerSwitch: ${address.url} is not trusted by the system; " +
                        "bootstrapping its CA with a one-shot trust-all connection"
            )
        }

        // 2. Self-signed (or otherwise untrusted) instance: fetch the CA anyway, but bind it to the
        //    certificate that was actually presented. The verifier/hostname check is disabled for
        //    this single bootstrap request only; nothing here becomes a process-wide trust anchor.
        val (pem, leaf) = try {
            httpsGet(caUrl, trustAll = true)
        }
        catch (t: Throwable) {
            return BootstrapResult(error = readableError(t))
        }
        val profile = ServerProfileStore.buildPrivateProfile(address.url, pem)
            ?: return BootstrapResult(error = "The server did not return a valid CA certificate.")
        val ca = ServerProfileStore.parseFirstCertificate(pem)
            ?: return BootstrapResult(error = "The server did not return a valid CA certificate.")
        if (leaf == null) {
            return BootstrapResult(error = "The server did not present a certificate.")
        }
        try {
            leaf.verify(ca.publicKey)
        }
        catch (t: Throwable) {
            Log.w(TAG, "ServerSwitch: the published CA did not sign the presented certificate", t)
            return BootstrapResult(error = "The server certificate is not signed by the CA it publishes.")
        }

        val pin = input.sha256Pin
        if (pin != null) {
            return if (ServerProfileStore.fingerprintMatches(pin, profile.caSha256)) {
                BootstrapResult(profile = profile)
            }
            else {
                BootstrapResult(error = "The CA fingerprint does not match the #sha256= value in the address.")
            }
        }
        return BootstrapResult(profile = profile, needsConfirmation = true)
    }

    private fun httpsGet(url: String, trustAll: Boolean): Pair<String, X509Certificate?> {
        val connection = URL(url).openConnection() as HttpsURLConnection
        connection.connectTimeout = TIMEOUT_MS
        connection.readTimeout = TIMEOUT_MS
        connection.instanceFollowRedirects = false
        if (trustAll) {
            val trustManager = object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit
                override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit
                override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            }
            val context = SSLContext.getInstance("TLS")
            context.init(null, arrayOf<TrustManager>(trustManager), SecureRandom())
            connection.sslSocketFactory = context.socketFactory
            connection.hostnameVerifier = HostnameVerifier { _, _ -> true }
        }
        try {
            val code = connection.responseCode
            if (code !in 200..299) {
                throw IOException("HTTP $code from $url")
            }
            val leaf = connection.serverCertificates?.firstOrNull() as? X509Certificate
            return readBody(connection) to leaf
        }
        finally {
            connection.disconnect()
        }
    }

    private fun httpGet(url: String): String {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = TIMEOUT_MS
        connection.readTimeout = TIMEOUT_MS
        connection.instanceFollowRedirects = true
        try {
            val code = connection.responseCode
            if (code !in 200..299) {
                throw IOException("HTTP $code from $url")
            }
            return readBody(connection)
        }
        finally {
            connection.disconnect()
        }
    }

    private fun readBody(connection: URLConnection): String =
        connection.inputStream.bufferedReader(Charsets.UTF_8).use {
            val buffer = CharArray(65537)
            var count = 0
            while (count < buffer.size) {
                val read = it.read(buffer, count, buffer.size - count)
                if (read < 0) break
                count += read
            }
            if (count > 65536) throw IOException("Server response is too large")
            String(buffer, 0, count)
        }

    private fun isTlsTrustError(t: Throwable): Boolean {
        var current: Throwable? = t
        var depth = 0
        while (current != null && depth < 8) {
            if (current is SSLException || current is CertificateException) {
                return true
            }
            current = current.cause
            depth++
        }
        return false
    }

    private fun readableError(t: Throwable): String {
        val message = t.message?.takeIf { it.isNotBlank() } ?: t.javaClass.simpleName
        return "Cannot reach the server: $message"
    }

    // ---- confirmation and saving ---------------------------------------------------------

    private fun confirmFingerprint(
        activity: Activity,
        profile: ServerProfile,
        listDialog: AlertDialog
    ) {
        if (activity.isFinishing || activity.isDestroyed) {
            Log.i(TAG, "ServerSwitch: the activity is gone; skipping the fingerprint dialog")
            return
        }
        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(activity, 20), dp(activity, 8), dp(activity, 20), 0)
        }
        container.addView(TextView(activity).apply {
            text = "Verify this fingerprint matches the one your server prints at startup:"
            textSize = 14f
        }, matchWidth())
        container.addView(TextView(activity).apply {
            text = shortFingerprint(profile.caSha256)
            textSize = 20f
            typeface = Typeface.MONOSPACE
            setPadding(0, dp(activity, 10), 0, dp(activity, 10))
        }, matchWidth())
        container.addView(TextView(activity).apply {
            text = profile.caSha256
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.GRAY)
        }, matchWidth())

        try {
            AlertDialog.Builder(activity)
                .setTitle("Trust this server?")
                .setView(ScrollView(activity).apply { addView(container) })
                .setPositiveButton("Trust") { d, _ ->
                    d.dismiss()
                    acceptProfile(activity, profile, listDialog)
                }
                .setNegativeButton("Cancel") { d, _ ->
                    d.dismiss()
                    restartList(activity, listDialog)
                }
                .setOnCancelListener { restartList(activity, listDialog) }
                .show()
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: cannot show the fingerprint dialog", t)
        }
    }

    private fun acceptProfile(activity: Activity, profile: ServerProfile, listDialog: AlertDialog) {
        try {
            ServerProfileStore.upsertProfile(activity.filesDir, profile)
            Log.i(
                TAG,
                "ServerSwitch: saved profile ${profile.id} (${profile.url}), " +
                        "fingerprint ${profile.caSha256}"
            )
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: cannot save the profile", t)
            showBootstrapError(activity, "Cannot save the server profile: ${t.message}", "", listDialog)
            return
        }
        Toast.makeText(activity, "Added ${profile.label}", Toast.LENGTH_SHORT).show()
        restartList(activity, listDialog)
    }

    private fun showBootstrapError(
        activity: Activity,
        message: String,
        rawText: String,
        listDialog: AlertDialog
    ) {
        if (activity.isFinishing || activity.isDestroyed) {
            Log.i(TAG, "ServerSwitch: the activity is gone; skipping the bootstrap error dialog")
            return
        }
        var retry = false
        try {
            AlertDialog.Builder(activity)
                .setTitle("Select server")
                .setMessage(message)
                .setPositiveButton("Retry") { d, _ ->
                    retry = true
                    d.dismiss()
                }
                .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
                .setOnDismissListener {
                    if (retry) promptForUrl(activity, listDialog, initial = rawText)
                    else restartList(activity, listDialog)
                }
                .show()
        }
        catch (t: Throwable) {
            Log.e(TAG, "ServerSwitch: cannot show the bootstrap error dialog", t)
        }
    }

    private fun shortFingerprint(fingerprint: String): String =
        fingerprint.split(':').take(8).joinToString(":")

    private fun matchWidth(): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)

    private fun dp(activity: Activity, value: Int): Int =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            value.toFloat(),
            activity.resources.displayMetrics
        ).toInt()
}
