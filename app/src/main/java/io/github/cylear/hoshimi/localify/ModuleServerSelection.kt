package io.github.cylear.hoshimi.localify

import android.app.Activity
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.os.Binder
import android.os.Bundle
import android.util.Log
import kotlinx.serialization.json.Json
import java.io.File

/** Server selection and account deletion bridge. Account files and tokens never leave the game's sandbox. */
class ServerSelectionProvider : ContentProvider() {
    override fun onCreate() = true
    override fun call(method: String, arg: String?, extras: Bundle?): Bundle {
        val ctx = requireNotNull(context)
        val callers = ctx.packageManager.getPackagesForUid(Binder.getCallingUid()).orEmpty()
        check(callers.any { it == BuildConfig.APPLICATION_ID || it in TargetGamePackages.all }) {
            "Caller is not a supported game"
        }
        if (method == "deletions") {
            val requests = ServerProfileStore.pendingAccountDeletions(ctx.filesDir)
                .filter { it.gamePackage in callers }
            return Bundle().apply { putString("requests", Json.encodeToString(
                kotlinx.serialization.builtins.ListSerializer(AccountDeletionRequest.serializer()), requests)) }
        }
        if (method == "ackDeletion") {
            requireNotNull(arg)
            callers.filter { it in TargetGamePackages.all }.forEach {
                ServerProfileStore.acknowledgeAccountDeletion(ctx.filesDir, arg, it)
            }
            return Bundle()
        }
        require(method == "selected")
        val id = ServerProfileStore.readActiveId(ctx.filesDir) ?: return Bundle()
        val profile = ServerProfileStore.loadProfiles(ctx.filesDir).first { it.id == id }
        return Bundle().apply { putString("profile", Json.encodeToString(ServerProfile.serializer(), profile)) }
    }
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException()
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = throw UnsupportedOperationException()
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = throw UnsupportedOperationException()
}

object ModuleServerSelection {
    private val uri = Uri.parse("content://${BuildConfig.APPLICATION_ID}.servers")
    private var restarting = false

    private fun selected(context: Context): ServerProfile? {
        val encoded = try {
            context.contentResolver.call(uri, "selected", null, null)?.getString("profile")
        } catch (failure: Exception) {
            // An embedded module can run without the management app. Retain its last server.
            Log.w(TAG, "Module server selection unavailable; retaining game selection", failure)
            return null
        } ?: return null
        val profile = Json.decodeFromString(ServerProfile.serializer(), encoded)
        require(profile.id == ServerProfile.OFFICIAL_ID ||
            (profile.kind == ServerProfile.KIND_PRIVATE &&
             ServerProfileStore.buildPrivateProfile(profile.url, profile.caPem)?.let {
                 it.id == profile.id && it.caSha256 == profile.caSha256
             } == true)) { "Invalid selected server" }
        return profile
    }

    private fun pendingDeletions(context: Context): List<AccountDeletionRequest> {
        val encoded = context.contentResolver.call(uri, "deletions", null, null)
            ?.getString("requests") ?: return emptyList()
        return Json.decodeFromString(kotlinx.serialization.builtins.ListSerializer(
            AccountDeletionRequest.serializer()), encoded)
    }

    fun prepareStartup(context: Context) {
        val dataDir = File(context.applicationInfo.dataDir)
        val filesDir = context.filesDir
        ServerProfileStore.configureExternalAccountStorage(
            requireNotNull(context.getExternalFilesDir(null)) { "Game external storage unavailable" })
        ServerProfileStore.migrateSharedExternalAccount(dataDir)
        // Complete any interrupted transaction before accepting a newer selection.
        ServerProfileStore.swapPendingAccount(dataDir)
        check(ServerProfileStore.readPendingTarget(filesDir) == null) { "Account recovery pending" }
        val profile = selected(context) ?: return
        for (request in pendingDeletions(context)) {
            require(request.gamePackage == context.packageName)
            require(request.profileId.matches(Regex("private-[0-9a-f]{8,64}")))
            if (ServerProfileStore.readActiveId(filesDir) == request.profileId) {
                check(ServerProfileStore.writePendingTarget(filesDir, ServerProfile.OFFICIAL_ID))
                ServerProfileStore.swapPendingAccount(dataDir)
            }
            ServerProfileStore.deleteSavedAccount(filesDir, request.profileId)
            context.contentResolver.call(uri, "ackDeletion", request.requestId, null)
        }
        ServerProfileStore.upsertProfile(filesDir, profile)
        val current = ServerProfileStore.readActiveId(filesDir) ?: ServerProfile.OFFICIAL_ID
        if (current != profile.id) {
            check(ServerProfileStore.writePendingTarget(filesDir, profile.id))
            ServerProfileStore.swapPendingAccount(dataDir)
            check(ServerProfileStore.readPendingTarget(filesDir) == null) { "Account switch pending" }
        } else if (!ServerProfileStore.hasActiveId(filesDir)) {
            ServerProfileStore.writeActiveId(filesDir, current)
        }
    }

    fun restartIfChanged(activity: Activity): Boolean {
        if (restarting) return true
        val profile = selected(activity) ?: return false
        val current = ServerProfileStore.readActiveId(activity.filesDir) ?: ServerProfile.OFFICIAL_ID
        val old = ServerProfileStore.activePrivateProfile(activity.filesDir)
        ServerProfileStore.upsertProfile(activity.filesDir, profile)
        if (current == profile.id && (old == null || old.caSha256 == profile.caSha256) &&
            pendingDeletions(activity).isEmpty()) return false
        restarting = true
        ServerProfileStore.requestSwitch(activity, profile.id)
        return true
    }
}
