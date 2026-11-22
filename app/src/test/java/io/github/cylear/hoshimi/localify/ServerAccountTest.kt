package io.github.cylear.hoshimi.localify

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class ServerAccountTest {
    @get:Rule val temporary = TemporaryFolder()
    private val prefs = "shared_prefs/com.google.firebase.auth.api.Store.test.xml"
    private val player = "shared_prefs/game.qualiarts.idolypride.v2.playerprefs.xml"
    private fun put(root: File, path: String, value: String) = File(root, path).apply {
        parentFile!!.mkdirs(); writeText(value)
    }
    private fun switch(root: File, id: String) {
        val files = File(root, "files")
        assertTrue(ServerProfileStore.writePendingTarget(files, id))
        ServerProfileStore.swapPendingAccount(root)
        assertNull(ServerProfileStore.readPendingTarget(files))
        assertEquals(id, ServerProfileStore.readActiveId(files))
    }

    @Test fun officialAndTwoServersRetainSeparateAccounts() {
        val root = temporary.newFolder()
        put(root, prefs, "official-token")
        put(root, "$prefs.bak", "official-backup")
        put(root, player, "official-player")
        put(root, "files/octo/user/account", "official-octo")
        put(root, "files/assets/keep", "shared-assets")
        switch(root, "server-a")
        assertFalse(File(root, prefs).exists())
        assertFalse(File(root, "$prefs.bak").exists())
        put(root, prefs, "a-token")
        switch(root, "server-b")
        assertFalse(File(root, prefs).exists())
        put(root, prefs, "b-token")
        switch(root, "server-a")
        assertEquals("a-token", File(root, prefs).readText())
        assertFalse(File(root, player).exists())
        switch(root, "official")
        assertEquals("official-token", File(root, prefs).readText())
        assertEquals("official-backup", File(root, "$prefs.bak").readText())
        assertEquals("official-player", File(root, player).readText())
        assertEquals("official-octo", File(root, "files/octo/user/account").readText())
        assertEquals("shared-assets", File(root, "files/assets/keep").readText())
    }

    @Test fun interruptedRestoreRemovesPartialFilesAndKeepsBackup() {
        val root = temporary.newFolder()
        val files = File(root, "files")
        put(root, "files/hoshimi-profiles/server-a/account/$prefs", "a-token")
        put(root, player, "partial-previous-account")
        put(root, "files/hoshimi-profiles/pending.json",
            """{"targetId":"server-a","fromId":"official","phase":"restoring","attempts":1}""")
        ServerProfileStore.swapPendingAccount(root)
        assertEquals("a-token", File(root, prefs).readText())
        assertFalse(File(root, player).exists())
        assertTrue(File(ServerProfileStore.accountDir(files, "server-a"), prefs).exists())
        assertNull(ServerProfileStore.readPendingTarget(files))
    }

    @Test fun externalUserIdAndRefreshBackupAreSeparatedAndRestored() {
        val root = temporary.newFolder()
        val external = temporary.newFolder()
        ServerProfileStore.configureExternalAccountStorage(external)
        val save = "43781DB5C40ECC39FD718685594F0956"
        val user = "$save/86D49E34E0FB5F1B10923B338B238345"
        val refresh = "$save/D2A57DC1D883FD21FB9951699DF71CC7/FCFD5A8259D1F6DDF530DE9A202E653C"
        put(external, user, "official-public-user-id")
        put(external, refresh, "official-refresh-backup")
        put(external, "asset-cache/keep", "shared")
        switch(root, "server-a")
        assertFalse(File(external, user).exists())
        assertFalse(File(external, refresh).exists())
        put(external, user, "private-public-user-id")
        put(external, refresh, "private-refresh-backup")
        switch(root, "official")
        assertEquals("official-public-user-id", File(external, user).readText())
        assertEquals("official-refresh-backup", File(external, refresh).readText())
        switch(root, "server-a")
        assertEquals("private-public-user-id", File(external, user).readText())
        assertEquals("private-refresh-backup", File(external, refresh).readText())
        assertEquals("shared", File(external, "asset-cache/keep").readText())
    }

    @Test fun legacySharedExternalAccountIsPreservedAndRemovedFromActivePrivateServer() {
        val root = temporary.newFolder()
        val external = temporary.newFolder()
        ServerProfileStore.configureExternalAccountStorage(external)
        val path = "43781DB5C40ECC39FD718685594F0956/user"
        put(external, path, "original-official-id")
        ServerProfileStore.writeActiveId(File(root, "files"), "server-a")
        ServerProfileStore.migrateSharedExternalAccount(root)
        assertFalse(File(external, path).exists())
        assertEquals("original-official-id", File(root,
            "files/hoshimi-profiles/legacy-external-account/user").readText())
        put(external, path, "private-id")
        ServerProfileStore.migrateSharedExternalAccount(root)
        assertEquals("private-id", File(external, path).readText())
        switch(root, "official")
        assertEquals("original-official-id", File(external, path).readText())
    }

    @Test fun accountDeletionRemovesOnlyRequestedInactiveStashAndAcknowledgesExactRequest() {
        val root = temporary.newFolder()
        val files = File(root, "files")
        val id = "private-01234567"
        ServerProfileStore.writeActiveId(files, "official")
        for (name in listOf("account", "account.new", "account.old"))
            put(root, "files/hoshimi-profiles/$id/$name/secret", "private-data")
        put(root, "files/hoshimi-profiles/official/account/keep", "official-data")
        put(root, "files/hoshimi-profiles/private-abcdef12/account/keep", "other-data")
        ServerProfileStore.requestAccountDeletion(files, ServerProfile(id, label = "test"),
            TargetGamePackages.IDOLY_PRIDE)
        val request = ServerProfileStore.pendingAccountDeletions(files).single()
        ServerProfileStore.deleteSavedAccount(files, id)
        assertFalse(ServerProfileStore.accountDir(files, id).exists())
        assertFalse(File(ServerProfileStore.profilesDir(files), "$id/account.old").exists())
        assertTrue(File(ServerProfileStore.accountDir(files, "official"), "keep").exists())
        assertTrue(File(ServerProfileStore.accountDir(files, "private-abcdef12"), "keep").exists())
        ServerProfileStore.acknowledgeAccountDeletion(files, request.requestId, TargetGamePackages.IDOLY_PRIDE_KR)
        assertEquals(1, ServerProfileStore.pendingAccountDeletions(files).size)
        ServerProfileStore.acknowledgeAccountDeletion(files, request.requestId, TargetGamePackages.IDOLY_PRIDE)
        assertTrue(ServerProfileStore.pendingAccountDeletions(files).isEmpty())
        assertThrows(IllegalArgumentException::class.java) { ServerProfileStore.deleteSavedAccount(files, "official") }
        assertThrows(IllegalArgumentException::class.java) { ServerProfileStore.deleteSavedAccount(files, "../escape") }
        ServerProfileStore.writeActiveId(files, id)
        assertThrows(IllegalStateException::class.java) { ServerProfileStore.deleteSavedAccount(files, id) }
    }

    @Test fun removingSelectedModuleProfileSelectsOfficialWhenRequested() {
        val root = temporary.newFolder()
        val files = File(root, "files")
        val profile = ServerProfile("private-01234567", url = "https://example.test")
        ServerProfileStore.upsertProfile(files, profile)
        ServerProfileStore.writeActiveId(files, profile.id)
        assertFalse(ServerProfileStore.removeProfile(files, profile.id))
        assertEquals(profile.id, ServerProfileStore.readActiveId(files))
        assertTrue(ServerProfileStore.removeProfile(files, profile.id, selectOfficialIfActive = true))
        assertEquals("official", ServerProfileStore.readActiveId(files))
        assertFalse(ServerProfileStore.loadProfiles(files).any { it.id == profile.id })
    }

    @Test fun corruptJournalStopsRecoveryWithoutTouchingAccount() {
        val root = temporary.newFolder()
        put(root, prefs, "keep")
        put(root, "files/hoshimi-profiles/pending.json", "broken")
        assertThrows(java.io.IOException::class.java) { ServerProfileStore.swapPendingAccount(root) }
        assertEquals("keep", File(root, prefs).readText())
    }
}
