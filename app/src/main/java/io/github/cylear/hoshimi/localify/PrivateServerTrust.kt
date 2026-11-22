package io.github.cylear.hoshimi.localify

import android.app.Activity
import android.app.AndroidAppHelper
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import kotlinx.serialization.json.Json
import java.io.ByteArrayInputStream
import java.io.File
import java.lang.reflect.Method
import java.security.KeyStore
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager

/**
 * Adds the private server CA to the trust anchors that libunity.so reports to UnityTLS.
 *
 * libunity.so builds the root list for UnityWebRequest inside libcurl/UnityTLS over JNI (verified
 * against the shipping binary, see the JNI strings below):
 *
 *   KeyStore.getInstance("AndroidCAStore").load((LoadStoreParameter) null)
 *     -> TrustManagerFactory.getInstance(<algorithm>).init(keyStore)
 *     -> TrustManagerFactory.getTrustManagers()
 *     -> ((X509TrustManager) tm).getAcceptedIssuers()
 *     -> X509Certificate.getEncoded() for every entry
 *
 * UnityTLS then treats those encodings as its own trust anchors, so the certificate chain of our
 * private server is accepted without the CA being present in the Android system store (which we
 * cannot write to on an unrooted, LSPatch-patched device).
 *
 * Scope of the change:
 *  - We only *append* certificates to the array returned by getAcceptedIssuers(), and only the CA
 *    configured for the private server is appended. Nothing else is added.
 *  - That augmented array is visible process-wide inside the game process: every consumer of the
 *    TrustManagerFactory in that process sees it, because the same X509TrustManager instance is
 *    shared. In practice that is libunity.so (UnityTLS/UnityWebRequest reads it over JNI).
 *  - Java-side validation is unaffected: getAcceptedIssuers() is only the list of *candidate*
 *    issuers; the actual chain check still goes through checkServerTrusted(), which we do not touch.
 *    No host is accepted that was not accepted before, and there is no accept-all path here.
 *
 * Trust state is driven exclusively by the configuration delivered to the *game* process (launch
 * intent, the `requestConfig` response, or a later intent). The module app never writes into the
 * game's filesDir, so the cache read on a direct launch is only ever a fallback for the window
 * before the config arrives; it is honoured only when it carries an "enabled" marker, and it is
 * dropped as soon as a disabled/blank configuration is observed.
 */
object PrivateServerTrust {
    private const val CACHE_FILE_NAME = "hoshimi-private-server-ca.pem"
    private const val CACHE_FORMAT_HEADER = "# hoshimi-private-server-ca-cache v1"
    private const val CACHE_ENABLED_PREFIX = "# enabled="
    private const val CACHE_SAVED_AT_PREFIX = "# savedAt="
    private const val CACHE_URL_PREFIX = "# url="
    private const val CACHE_SOURCE_PREFIX = "# source="
    /** Cache entry written from an in-game server profile (deleted when the active profile is official). */
    private const val CACHE_SOURCE_PROFILE = "profile"
    /** Cache entry written from a configuration supplied by the module app (previous behaviour). */
    private const val CACHE_SOURCE_APP = "app"
    private const val REQUEST_CONFIG_MARKER = "requestConfig"
    private const val AUGMENT_LOG_INTERVAL_MS = 2000L

    /**
     * A cache entry is only ever a hint for the next direct launch. If no configuration has
     * corroborated it for this long, the module app is gone (or never launched) and the private
     * server cannot be configured at all, so the entry is discarded instead of being trusted
     * forever.
     */
    private const val CACHE_MAX_AGE_MS = 30L * 24 * 60 * 60 * 1000
    private const val CACHE_REFRESH_INTERVAL_MS = 60L * 60 * 1000

    /** Lenient on purpose: a config written by a newer/older module build must never break trust. */
    private val configJson = Json { ignoreUnknownKeys = true }
    private val pemBlockRegex = Regex(
        "-----BEGIN CERTIFICATE-----[\\s\\S]*?-----END CERTIFICATE-----"
    )

    private val installed = AtomicBoolean(false)
    private val hookedAcceptedIssuersMethods = ConcurrentHashMap.newKeySet<Method>()
    private val classesWithoutAcceptedIssuers = ConcurrentHashMap.newKeySet<Class<*>>()

    /** Last time the getTrustManagers log line was emitted, per observed trust manager class set. */
    private val trustManagerLogStates = ConcurrentHashMap<String, Long>()

    @Volatile
    private var caCerts: List<X509Certificate> = emptyList()

    /**
     * Private server URL that belongs to [caCerts]; the Java-side redirect
     * ([PrivateServerRedirect]) needs it to know where to send the Firebase hosts.
     */
    @Volatile
    private var serverUrl: String = ""

    @Volatile
    private var cacheFileRead = false

    @Volatile
    private var lastAugmentLogAt = 0L

    // ---- in-process consumers --------------------------------------------------------------

    /**
     * Base URL of the enabled private server, as delivered by the configuration ("" when disabled).
     * A launcher start has no `iprData`, so the cached CA is consulted first; without it the fake
     * server URL would be unknown even though the cache proved the server was enabled.
     */
    internal fun currentServerUrl(): String {
        ensureCachedCaLoaded("narrative")
        return serverUrl
    }

    /**
     * TLS socket factory that trusts exactly the configured private server CA, used by
     * [NarrativeNotifier] to call the mod endpoints of the private server. Null when no CA is
     * configured.
     */
    internal fun sslSocketFactory(): SSLSocketFactory? {
        val certs = caCerts
        if (certs.isEmpty()) {
            return null
        }
        return try {
            val keyStore = KeyStore.getInstance(KeyStore.getDefaultType())
            keyStore.load(null, null)
            certs.forEachIndexed { index, cert ->
                keyStore.setCertificateEntry("hoshimi-private-server-$index", cert)
            }
            val factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
            factory.init(keyStore)
            val context = SSLContext.getInstance("TLS")
            context.init(null, factory.trustManagers, null)
            context.socketFactory
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot build the private server SSLSocketFactory", t)
            null
        }
    }

    /**
     * Installs the hooks that are independent of the configuration. Must be called from
     * handleLoadPackage(), i.e. before anything in the game process can touch TLS.
     */
    fun install(classLoader: ClassLoader) {
        if (!installed.compareAndSet(false, true)) {
            return
        }
        hookTrustManagerFactory(classLoader)
        hookActivityLaunch(classLoader)
        // Firebase Auth is the Java SDK: its DNS lookups and Conscrypt handshakes never reach the
        // native hooks, so the Java-side redirects/trust fallback must be armed just as early.
        PrivateServerRedirect.install(classLoader)
    }

    /** Feeds the configuration carried by the launch intent, before the Unity player exists. */
    fun observeIntent(activity: Activity?) {
        if (activity == null) {
            return
        }
        val source = "intent/${activity.javaClass.simpleName}"
        observeIntent(activity.intent, source)
    }

    /**
     * Feeds an intent that is delivered after the activity already exists (the `requestConfig`
     * answer arrives through `onNewIntent` on a task-reused Unity activity, where `getIntent()`
     * still returns the stale launch intent).
     */
    fun observeIntent(intent: Intent?, source: String) {
        val raw = try {
            intent?.getStringExtra("iprData")
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot read the intent from $source", t)
            null
        }
        if (raw.isNullOrBlank()) {
            // Launched straight from the launcher: fall back to the CA cached by a previous start,
            // but only while the cache still proves that the private server was enabled.
            ensureCachedCaLoaded(source)
            return
        }
        observeConfigJson(raw, source)
    }

    /** Feeds an already received `iprData` JSON payload or parsed configuration. */
    fun observeConfigJson(raw: String?, source: String) {
        if (raw.isNullOrBlank() || raw == REQUEST_CONFIG_MARKER) {
            ensureCachedCaLoaded(source)
            return
        }
        val config = try {
            configJson.decodeFromString<IdolyprideConfig>(raw)
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot parse the config from $source", t)
            return
        }
        observeConfig(config, source)
    }

    fun observeConfig(config: IdolyprideConfig?, source: String) {
        if (config == null) {
            return
        }
        val context = appContext()
        val filesDir = context?.filesDir
        if (source.startsWith("standalone")) {
            // Built by initStandaloneConfig from the active in-game profile: it *is* the effective
            // configuration, so there is no app configuration to outrank it.
            ServerProfileStore.updateAppManaged(false)
            applyConfig(
                config.privateServerEnabled,
                config.privateServerUrl,
                config.privateServerCaPem,
                source,
                context,
                cacheSource = CACHE_SOURCE_PROFILE
            )
            return
        }

        // Precedence: an app configuration that enables the private server wins (unchanged
        // behaviour for the sumire-sr dev setup). Otherwise the in-game profile decides, and an
        // app payload with the private server off must never disarm an active private profile.
        val appEnabled = config.privateServerEnabled && config.privateServerUrl.isNotBlank()
        ServerProfileStore.updateAppManaged(appEnabled)
        if (appEnabled) {
            if (filesDir != null) {
                try {
                    ServerProfileStore.migrateFromAppConfig(filesDir, config)
                }
                catch (t: Throwable) {
                    Log.w(TAG, "PrivateServerTrust: cannot migrate the app configuration", t)
                }
            }
            applyConfig(
                true,
                config.privateServerUrl,
                config.privateServerCaPem,
                source,
                context,
                cacheSource = CACHE_SOURCE_APP
            )
            return
        }

        val profile = if (filesDir == null) {
            null
        }
        else {
            try {
                ServerProfileStore.activePrivateProfile(filesDir)
            }
            catch (t: Throwable) {
                Log.w(TAG, "PrivateServerTrust: cannot read the active server profile", t)
                null
            }
        }
        if (profile != null) {
            applyConfig(
                true,
                profile.url,
                profile.caPem,
                "profile/$source",
                context,
                cacheSource = CACHE_SOURCE_PROFILE
            )
            return
        }
        applyConfig(false, "", "", source, context, cacheSource = CACHE_SOURCE_APP)
    }

    // ---- hooks ---------------------------------------------------------------------------

    /**
     * Unity resolves the root list through TrustManagerFactory, so this is the narrowest reliable
     * point: it tells us the concrete X509TrustManager classes that Unity is about to ask, and we
     * hook getAcceptedIssuers on each of them exactly once.
     */
    private fun hookTrustManagerFactory(classLoader: ClassLoader) {
        try {
            val factoryClass = XposedHelpers.findClass("javax.net.ssl.TrustManagerFactory", classLoader)
            val hookCount = XposedBridge.hookAllMethods(factoryClass, "getTrustManagers", object : XC_MethodHook() {
                override fun afterHookedMethod(param: MethodHookParam) {
                    try {
                        // Oldest hook point that can still see a cached CA on a direct launch.
                        ensureCachedCaLoaded("getTrustManagers")

                        val managers = (param.result as? Array<*>)
                            ?.filterIsInstance<X509TrustManager>()
                            ?: return
                        var hooked = 0
                        for (manager in managers) {
                            if (hookAcceptedIssuers(manager.javaClass)) {
                                hooked++
                            }
                        }
                        logTrustManagers(managers, hooked)
                    }
                    catch (t: Throwable) {
                        Log.w(TAG, "PrivateServerTrust: getTrustManagers hook failed", t)
                    }
                }
            })
            Log.i(TAG, "PrivateServerTrust: hooked TrustManagerFactory.getTrustManagers ($hookCount method(s))")
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot hook TrustManagerFactory.getTrustManagers", t)
        }
    }

    /**
     * getTrustManagers runs for every keystore/algorithm pair and is called often enough to spam
     * the log. The first call for a given set of trust manager classes is always logged; repeats
     * are throttled exactly like the augmentation log.
     */
    private fun logTrustManagers(managers: List<X509TrustManager>, hooked: Int) {
        val key = managers.map { it.javaClass.name }.distinct().sorted().joinToString(",")
            .ifEmpty { "<none>" }
        val now = SystemClock.elapsedRealtime()
        val firstForClasses = trustManagerLogStates.putIfAbsent(key, now) == null
        if (!firstForClasses) {
            val last = trustManagerLogStates[key] ?: now
            if (now - last < AUGMENT_LOG_INTERVAL_MS) {
                return
            }
            trustManagerLogStates[key] = now
        }
        Log.i(
            TAG,
            "PrivateServerTrust: TrustManagerFactory.getTrustManagers -> " +
                    "${managers.size} X509TrustManager(s) [$key], newly hooked $hooked, " +
                    "caCerts=${caCerts.size}"
        )
    }

    /**
     * The configuration reaches the game process as an Intent extra, so the earliest point where
     * the PEM is readable is right before the Unity activity is created (callActivityOnCreate runs
     * before onCreate, and therefore before UnityPlayer starts the runtime and its networking).
     *
     * `onNewIntent` covers the `requestConfig` answer: the module app starts the already running
     * Unity activity to hand over the current configuration, and that intent never becomes
     * `Activity.getIntent()`, so without this hook the fresh (possibly disabled) config would only
     * be seen if the activity were recreated.
     */
    private fun hookActivityLaunch(classLoader: ClassLoader) {
        try {
            XposedHelpers.findAndHookMethod(
                "android.app.Instrumentation",
                classLoader,
                "callActivityOnCreate",
                Activity::class.java,
                Bundle::class.java,
                object : XC_MethodHook() {
                    override fun beforeHookedMethod(param: MethodHookParam) {
                        try {
                            observeIntent(param.args[0] as? Activity)
                        }
                        catch (t: Throwable) {
                            Log.w(TAG, "PrivateServerTrust: intent pickup failed", t)
                        }
                    }
                }
            )
            Log.i(TAG, "PrivateServerTrust: hooked Instrumentation.callActivityOnCreate for early CA pickup")
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot hook Instrumentation.callActivityOnCreate", t)
        }

        try {
            XposedHelpers.findAndHookMethod(
                "android.app.Activity",
                classLoader,
                "onNewIntent",
                Intent::class.java,
                object : XC_MethodHook() {
                    override fun beforeHookedMethod(param: MethodHookParam) {
                        try {
                            val activity = param.thisObject as? Activity
                            observeIntent(
                                param.args[0] as? Intent,
                                "onNewIntent/${activity?.javaClass?.simpleName ?: "unknown"}"
                            )
                        }
                        catch (t: Throwable) {
                            Log.w(TAG, "PrivateServerTrust: onNewIntent pickup failed", t)
                        }
                    }
                }
            )
            Log.i(TAG, "PrivateServerTrust: hooked Activity.onNewIntent for config updates")
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot hook Activity.onNewIntent", t)
        }
    }

    /** Returns true only when a new getAcceptedIssuers() implementation was hooked. */
    private fun hookAcceptedIssuers(trustManagerClass: Class<*>): Boolean {
        val method = findAcceptedIssuersMethod(trustManagerClass)
        if (method == null) {
            if (classesWithoutAcceptedIssuers.add(trustManagerClass)) {
                Log.w(
                    TAG,
                    "PrivateServerTrust: ${trustManagerClass.name} exposes no getAcceptedIssuers()"
                )
            }
            return false
        }
        if (!hookedAcceptedIssuersMethods.add(method)) {
            return false
        }
        return try {
            XposedBridge.hookMethod(method, object : XC_MethodHook() {
                override fun afterHookedMethod(param: MethodHookParam) {
                    try {
                        augmentAcceptedIssuers(param)
                    }
                    catch (t: Throwable) {
                        Log.w(TAG, "PrivateServerTrust: getAcceptedIssuers hook failed", t)
                    }
                }
            })
            Log.i(
                TAG,
                "PrivateServerTrust: hooked X509TrustManager.getAcceptedIssuers on " +
                        "${trustManagerClass.name} (declared by ${method.declaringClass?.name})"
            )
            true
        }
        catch (t: Throwable) {
            Log.w(
                TAG,
                "PrivateServerTrust: cannot hook getAcceptedIssuers on ${trustManagerClass.name}", t
            )
            false
        }
    }

    private fun findAcceptedIssuersMethod(clazz: Class<*>): Method? {
        var current: Class<*>? = clazz
        while (current != null && current != Any::class.java) {
            val found = try {
                current.getDeclaredMethod("getAcceptedIssuers")
            }
            catch (_: Throwable) {
                null
            }
            if (found != null) {
                return found
            }
            current = current.superclass
        }
        return null
    }

    private fun augmentAcceptedIssuers(param: XC_MethodHook.MethodHookParam) {
        val certs = caCerts
        if (certs.isEmpty()) {
            return
        }
        val current = (param.result as? Array<*>)?.filterIsInstance<X509Certificate>() ?: return
        val missing = certs.filter { ours -> current.none { it.encoded.contentEquals(ours.encoded) } }
        if (missing.isEmpty()) {
            return
        }
        val merged = (current + missing).toTypedArray()
        param.result = merged
        logAugmented(param.thisObject?.javaClass?.name ?: "unknown", current.size, merged.size)
    }

    private fun logAugmented(className: String, before: Int, after: Int) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastAugmentLogAt < AUGMENT_LOG_INTERVAL_MS) {
            return
        }
        lastAugmentLogAt = now
        Log.i(
            TAG,
            "PrivateServerTrust: getAcceptedIssuers on $className augmented $before -> $after"
        )
    }

    // ---- configuration -------------------------------------------------------------------

    private fun applyConfig(
        enabled: Boolean,
        url: String,
        pem: String,
        source: String,
        context: Context?,
        updateCache: Boolean = true,
        cacheSource: String = CACHE_SOURCE_APP
    ) {
        if (!enabled || pem.isBlank()) {
            // A disabled or blank config is authoritative: drop the in-memory anchors and the cache
            // immediately, so neither this process nor the next direct launch keeps trusting a CA
            // the user has turned off.
            val hadCerts = caCerts.isNotEmpty()
            caCerts = emptyList()
            serverUrl = ""
            PrivateServerRedirect.onConfigCleared(source)
            val removed = deleteCachedCa(context)
            if (hadCerts || removed) {
                Log.i(
                    TAG,
                    "PrivateServerTrust: private server disabled/blank ($source); " +
                            "caCerts cleared (had=$hadCerts), cached CA " +
                            (if (removed) "deleted" else "absent")
                )
            }
            return
        }

        val parsed = parseCerts(pem)
        if (parsed.isEmpty()) {
            Log.e(TAG, "PrivateServerTrust: no X.509 certificate in the configured CA PEM ($source)")
            return
        }
        // The redirect needs both the URL and the CA; a legacy cache without the URL still gets the
        // trust anchors but leaves the Java redirect off.
        serverUrl = url.trim()
        PrivateServerRedirect.onConfigChanged(serverUrl, parsed, source)
        if (sameCerts(parsed, caCerts)) {
            if (updateCache) {
                writeCachedCa(context, serverUrl, pem, cacheSource)
            }
            return
        }

        caCerts = parsed
        Log.i(
            TAG,
            "PrivateServerTrust: private server CA loaded from $source: count=${parsed.size}, " +
                    "subjects=[${parsed.joinToString { it.subjectX500Principal.name }}]"
        )
        if (updateCache) {
            writeCachedCa(context, serverUrl, pem, cacheSource)
        }
    }

    private fun parseCerts(pem: String): List<X509Certificate> {
        return try {
            val factory = CertificateFactory.getInstance("X.509")
            val blocks = pemBlockRegex.findAll(pem).map { it.value }.toList()
            if (blocks.isEmpty()) {
                // Tolerate a bare base64 body without the BEGIN/END armour.
                val wrapped = "-----BEGIN CERTIFICATE-----\n$pem\n-----END CERTIFICATE-----"
                return listOfNotNull(decodeCert(factory, wrapped))
            }
            blocks.mapNotNull { decodeCert(factory, it) }
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: CA PEM parsing failed", t)
            emptyList()
        }
    }

    private fun decodeCert(factory: CertificateFactory, block: String): X509Certificate? {
        return try {
            factory.generateCertificate(
                ByteArrayInputStream(block.toByteArray(Charsets.US_ASCII))
            ) as? X509Certificate
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: skipping an unparsable PEM block", t)
            null
        }
    }

    private fun sameCerts(a: List<X509Certificate>, b: List<X509Certificate>): Boolean {
        if (a.size != b.size) {
            return false
        }
        val bEncoded = b.map { it.encoded }
        return a.all { aCert -> bEncoded.any { it.contentEquals(aCert.encoded) } }
    }

    // ---- cache ---------------------------------------------------------------------------

    private data class CachedCa(
        val pem: String,
        val url: String,
        val enabled: Boolean,
        val savedAt: Long,
        val source: String
    )

    /**
     * Launching the game straight from the launcher carries no `iprData` extra. The CA seen on a
     * previous (module initiated) launch is cached in the game's own files directory, which only
     * the game process can read or write — that is exactly what LSPatch integrated mode needs, and
     * the module app deliberately never touches it.
     *
     * The cache is *not* self-authorising: it only says "the last configuration this process
     * received had the private server enabled". It is therefore honoured only when it carries that
     * marker, it is discarded once it is older than [CACHE_MAX_AGE_MS], and [applyConfig] removes
     * it the moment a disabled or blank configuration arrives (normally within the first frame
     * after the `requestConfig` answer, see the onNewIntent hook).
     */
    private fun ensureCachedCaLoaded(source: String) {
        if (cacheFileRead) {
            return
        }
        cacheFileRead = true
        if (caCerts.isNotEmpty()) {
            return
        }
        val context = appContext()
        if (context == null) {
            cacheFileRead = false
            return
        }
        // An in-game profile is authoritative on a direct launch: arm its CA before UnityTLS reads
        // the root list, and keep the cached file in sync with the profile.
        val profile = try {
            ServerProfileStore.activePrivateProfile(context.filesDir)
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot read the active server profile", t)
            null
        }
        if (profile != null) {
            Log.i(
                TAG,
                "PrivateServerTrust: arming the CA of the active profile ${profile.id} " +
                        "(${profile.url}) before the first configuration arrives ($source)"
            )
            applyConfig(
                true,
                profile.url,
                profile.caPem,
                "profile/$source",
                context,
                cacheSource = CACHE_SOURCE_PROFILE
            )
            return
        }
        val file = File(context.filesDir, CACHE_FILE_NAME)
        // Once active.json exists, the profile store owns the decision. If it resolves to the
        // official server and the app has not enabled a private server in this process, the legacy
        // cache file must never arm trust (and is removed). The legacy fallback is only for the
        // pre-migration case where active.json does not exist yet.
        if (ServerProfileStore.hasActiveId(context.filesDir) &&
            !ServerProfileStore.appManagedPrivateServer
        ) {
            Log.i(
                TAG,
                "PrivateServerTrust: active.json selects a non-private server and the app has not " +
                        "enabled the private server; refusing the cached CA ($source)"
            )
            deleteCachedCa(file)
            return
        }
        val cached = readCachedCa(file, source) ?: return
        if (cached.source == CACHE_SOURCE_PROFILE) {
            // The profile this cache belongs to is no longer active (the player switched to the
            // official server); the app never wrote this entry, so it must not be trusted.
            Log.i(
                TAG,
                "PrivateServerTrust: the cached CA belongs to a profile that is no longer active; " +
                        "not trusted ($source)"
            )
            deleteCachedCa(file)
            return
        }
        if (!cached.enabled) {
            Log.i(
                TAG,
                "PrivateServerTrust: cached CA at ${file.absolutePath} is not marked enabled; " +
                        "not trusted ($source)"
            )
            deleteCachedCa(file)
            return
        }
        val now = System.currentTimeMillis()
        val age = if (cached.savedAt > 0L) now - cached.savedAt else -1L
        if (age > CACHE_MAX_AGE_MS) {
            Log.i(
                TAG,
                "PrivateServerTrust: cached CA at ${file.absolutePath} is stale " +
                        "(age=${age / 86_400_000L}d, limit=${CACHE_MAX_AGE_MS / 86_400_000L}d); " +
                        "not trusted ($source)"
            )
            deleteCachedCa(file)
            return
        }
        Log.i(
            TAG,
            "PrivateServerTrust: direct launch without config; using the enabled-marked CA " +
                    "cached at ${file.absolutePath} (age=${if (age >= 0L) "${age / 3_600_000L}h" else "unknown"}; " +
                    "will be revoked when a disabled config arrives)"
        )
        if (cached.url.isBlank()) {
            Log.i(
                TAG,
                "PrivateServerTrust: the cached CA carries no server URL (written by an older " +
                        "build); Java-side DNS/port redirect stays off ($source)"
            )
        }
        applyConfig(true, cached.url, cached.pem, "cache/$source", context, updateCache = false)
    }

    private fun readCachedCa(file: File, source: String): CachedCa? {
        if (!file.isFile) {
            return null
        }
        val text = try {
            file.readText()
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot read the cached CA", t)
            return null
        }
        if (text.isBlank()) {
            return null
        }
        var enabled = false
        var savedAt = 0L
        var url = ""
        var source = CACHE_SOURCE_APP
        var marked = false
        val pemLines = ArrayList<String>()
        for (line in text.lineSequence()) {
            val trimmed = line.trim()
            when {
                trimmed.startsWith(CACHE_ENABLED_PREFIX) -> {
                    enabled = trimmed.removePrefix(CACHE_ENABLED_PREFIX).trim()
                        .equals("true", ignoreCase = true)
                    marked = true
                }
                trimmed.startsWith(CACHE_SAVED_AT_PREFIX) -> {
                    savedAt = trimmed.removePrefix(CACHE_SAVED_AT_PREFIX).trim().toLongOrNull() ?: 0L
                    marked = true
                }
                trimmed.startsWith(CACHE_URL_PREFIX) -> {
                    // Optional: older caches have no URL, and old builds ignore this unknown header.
                    url = trimmed.removePrefix(CACHE_URL_PREFIX).trim()
                }
                trimmed.startsWith(CACHE_SOURCE_PREFIX) -> {
                    // Optional: caches written before in-game profiles existed came from the app.
                    source = trimmed.removePrefix(CACHE_SOURCE_PREFIX).trim()
                        .ifEmpty { CACHE_SOURCE_APP }
                }
                trimmed.startsWith("#") -> Unit // format header, ignored
                else -> pemLines.add(line)
            }
        }
        val pem = pemLines.joinToString("\n").trim()
        if (pem.isEmpty()) {
            return null
        }
        if (!marked) {
            // Written by the first version of this hook, which could not distinguish an enabled
            // from a disabled configuration. Unknown provenance is not trustworthy.
            Log.i(
                TAG,
                "PrivateServerTrust: ignoring the legacy cached CA at ${file.absolutePath} " +
                        "(no enabled marker; $source)"
            )
            deleteCachedCa(file)
            return null
        }
        return CachedCa(pem, url, enabled, savedAt, source)
    }

    private fun writeCachedCa(context: Context?, url: String, pem: String, source: String) {
        val ctx = context ?: return
        try {
            val file = File(ctx.filesDir, CACHE_FILE_NAME)
            val normalized = pem.trim()
            val normalizedUrl = url.trim()
            val now = System.currentTimeMillis()
            val existing = readCachedCa(file, "cache-write")
            if (existing != null &&
                existing.enabled &&
                existing.pem == normalized &&
                existing.url == normalizedUrl &&
                existing.source == source &&
                now - existing.savedAt in 0..CACHE_REFRESH_INTERVAL_MS
            ) {
                return
            }
            file.writeText(
                buildString {
                    append(CACHE_FORMAT_HEADER).append('\n')
                    append(CACHE_ENABLED_PREFIX).append("true").append('\n')
                    append(CACHE_SAVED_AT_PREFIX).append(now).append('\n')
                    if (normalizedUrl.isNotEmpty()) {
                        append(CACHE_URL_PREFIX).append(normalizedUrl).append('\n')
                    }
                    append(CACHE_SOURCE_PREFIX).append(source).append('\n')
                    append(normalized).append('\n')
                }
            )
            Log.i(
                TAG,
                "PrivateServerTrust: cached the CA for the next launch (enabled marker, " +
                        "${file.absolutePath})"
            )
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot cache the CA", t)
        }
    }

    private fun deleteCachedCa(context: Context?): Boolean {
        val ctx = context ?: return false
        return deleteCachedCa(File(ctx.filesDir, CACHE_FILE_NAME))
    }

    private fun deleteCachedCa(file: File): Boolean {
        return try {
            if (file.isFile && file.delete()) {
                Log.i(TAG, "PrivateServerTrust: removed the cached CA (${file.absolutePath})")
                true
            }
            else {
                false
            }
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: cannot remove the cached CA", t)
            false
        }
    }

    private fun appContext(): Context? {
        return try {
            AndroidAppHelper.currentApplication()
        }
        catch (t: Throwable) {
            Log.w(TAG, "PrivateServerTrust: no application context available", t)
            null
        }
    }
}
