package io.github.cylear.hoshimi.localify.ui.pages.subPages

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import io.github.cylear.hoshimi.localify.*
import io.github.cylear.hoshimi.localify.R
import io.github.cylear.hoshimi.localify.ui.components.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun PrivateServerPage(modifier: Modifier = Modifier, context: MainActivity? = null) {
    val uiContext = LocalContext.current
    fun tr(id: Int, vararg args: Any): String = uiContext.getString(id, *args)
    fun profiles() = context?.let { ServerProfileStore.loadProfiles(it.filesDir) }
        ?: listOf(ServerProfileStore.officialProfile())
    var servers by remember { mutableStateOf(profiles()) }
    var selected by remember { mutableStateOf(context?.let { ServerProfileStore.readActiveId(it.filesDir) }) }
    var adding by rememberSaveable { mutableStateOf(false) }
    var address by rememberSaveable { mutableStateOf("") }
    var nickname by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var pending by remember { mutableStateOf<ServerProfile?>(null) }
    var details by remember { mutableStateOf<ServerProfile?>(null) }
    var editedNickname by remember { mutableStateOf("") }
    var removing by remember { mutableStateOf<ServerProfile?>(null) }
    var deleteAccount by remember { mutableStateOf(false) }
    var removeServer by remember { mutableStateOf(true) }
    var deletionStatus by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    LaunchedEffect(context) {
        var wasPending = false
        while (context != null) {
            val requests = ServerProfileStore.pendingAccountDeletions(context.filesDir)
            deletionStatus = if (requests.isEmpty()) {
                if (wasPending) tr(R.string.ps_ui_01) else deletionStatus
            } else tr(R.string.ps_ui_02) + requests.joinToString { it.label } + tr(R.string.ps_ui_03)
            wasPending = requests.isNotEmpty()
            kotlinx.coroutines.delay(1500)
        }
    }

    fun save(profile: ServerProfile) {
        val activity = context ?: return
        try {
            ServerProfileStore.upsertProfile(activity.filesDir, profile)
            servers = profiles()
            pending = null
            adding = false
            address = ""
            nickname = ""
            error = null
        } catch (failure: Exception) { error = tr(R.string.ps_ui_04, failure.message.orEmpty()) }
    }

    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 80.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            IPGroupBox(title = tr(R.string.ps_ui_05)) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(tr(R.string.ps_ui_06),
                        style = MaterialTheme.typography.bodyMedium)
                    HorizontalDivider()
                    Text(tr(R.string.ps_ui_07), style = MaterialTheme.typography.bodyLarge)
                    servers.forEach { server ->
                        key(server.id) {
                            val official = server.kind == ServerProfile.KIND_OFFICIAL
                            val active = selected == server.id
                            IPRadio(modifier = Modifier.fillMaxWidth(),
                                text = "", selected = active,
                                leadingContent = {
                                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                        Text(if (official) tr(R.string.ps_ui_08) else server.label.ifBlank { server.url },
                                            style = MaterialTheme.typography.bodyLarge)
                                        if (!official) Text(server.url, style = MaterialTheme.typography.bodySmall)
                                    }
                                }, onClick = {
                                    context?.let { activity ->
                                        try {
                                            ServerProfileStore.writeActiveId(activity.filesDir, server.id)
                                            selected = server.id
                                            error = null
                                        } catch (failure: Exception) {
                                            error = tr(R.string.ps_ui_09, failure.message.orEmpty())
                                        }
                                    }
                                })
                        }
                    }
                    val activeServer = servers.firstOrNull { it.id == selected }
                    Text(tr(R.string.ps_ui_10) + when {
                        activeServer == null -> tr(R.string.ps_ui_11)
                        activeServer.kind == ServerProfile.KIND_OFFICIAL -> tr(R.string.ps_ui_08)
                        else -> activeServer.label.ifBlank { activeServer.url }
                    }, style = MaterialTheme.typography.bodyMedium)
                    IPButton(text = tr(R.string.ps_ui_12),
                        enabled = activeServer?.kind == ServerProfile.KIND_PRIVATE,
                        onClick = {
                            activeServer?.let { details = it; editedNickname = it.label; error = null }
                        }, modifier = Modifier.fillMaxWidth().height(40.dp))
                }
            }
        }
        item {
            IPButton(text = tr(R.string.ps_ui_13), onClick = { adding = true; error = null },
                enabled = context != null, modifier = Modifier.fillMaxWidth().height(44.dp))
        }
        item { if (deletionStatus.isNotEmpty()) Text(deletionStatus, style = MaterialTheme.typography.bodySmall) }
        item { error?.let { Text(it, color = MaterialTheme.colorScheme.error) } }
        if (selected == null) item {
            Text(tr(R.string.ps_ui_14),
                style = MaterialTheme.typography.bodySmall)
        }
    }

    fun dismissSheet() {
        if (busy) return
        when {
            pending != null -> { pending = null; adding = true }
            details != null -> details = null
            removing != null -> removing = null
            else -> adding = false
        }
    }

    if (adding || pending != null || details != null || removing != null) {
        Dialog(onDismissRequest = { dismissSheet() },
            properties = DialogProperties(usePlatformDefaultWidth = false, dismissOnBackPress = !busy,
                dismissOnClickOutside = !busy)) {
            val title = when {
                pending != null -> tr(R.string.ps_ui_15)
                details != null -> tr(R.string.ps_ui_12)
                removing != null -> if (removeServer) tr(R.string.ps_ui_16) else tr(R.string.ps_ui_17)
                else -> tr(R.string.ps_ui_18)
            }
            key(title) {
                IPGroupConfirm(title = title, baseModifier = Modifier.fillMaxSize(),
                    contentHeightForAnimation = 2000f,
                    confirmText = when {
                        pending != null -> tr(R.string.ps_ui_19)
                        details != null -> tr(R.string.ps_ui_20)
                        removing != null -> tr(R.string.ps_ui_21)
                        busy -> tr(R.string.ps_ui_22)
                        else -> tr(R.string.ps_ui_23)
                    },
                    confirmEnabled = !busy && (!adding || address.isNotBlank()),
                    cancelEnabled = !busy,
                    dismissOnConfirm = false,
                    onCancel = { dismissSheet() },
                    onConfirm = {
                        val certificate = pending
                        val deletion = removing
                        when {
                            certificate != null -> save(certificate)
                            details != null -> {
                                try {
                                    val profile = requireNotNull(details)
                                    ServerProfileStore.upsertProfile(requireNotNull(context).filesDir,
                                        profile.copy(label = editedNickname.trim().ifBlank { profile.url }.take(100)))
                                    servers = profiles()
                                    error = null
                                    details = null
                                } catch (failure: Exception) {
                                    error = tr(R.string.ps_ui_24, failure.message.orEmpty())
                                }
                            }
                            deletion != null -> {
                                try {
                                    val activity = requireNotNull(context)
                                    val gamePackage = if (deleteAccount) requireNotNull(TargetGamePackages.findInstalled(activity)) {
                                        tr(R.string.ps_ui_25)
                                    } else null
                                    if (removeServer) {
                                        check(ServerProfileStore.removeProfile(activity.filesDir, deletion.id,
                                            selectOfficialIfActive = true))
                                        selected = ServerProfileStore.readActiveId(activity.filesDir)
                                    }
                                    servers = profiles()
                                    if (gamePackage != null) ServerProfileStore.requestAccountDeletion(
                                        activity.filesDir, deletion, gamePackage)
                                    removing = null
                                } catch (failure: Exception) {
                                    error = tr(R.string.ps_ui_26, failure.message.orEmpty())
                                    removing = null
                                }
                            }
                            else -> {
                                val input = ServerProfileStore.parseServerInput(address.trim())
                                if (input == null || input.address.scheme != "https") {
                                    error = tr(R.string.ps_ui_27)
                                } else {
                                    busy = true
                                    error = null
                                    scope.launch {
                                        try {
                                            val result = withContext(Dispatchers.IO) { ServerPromptDialog.fetchCa(input) }
                                            val profile = result.profile?.let {
                                                it.copy(label = nickname.trim().ifBlank { it.label }.take(100))
                                            }
                                            when {
                                                result.error != null -> error = tr(R.string.ps_ui_28, result.error.orEmpty())
                                                profile == null -> error = tr(R.string.ps_ui_29)
                                                result.needsConfirmation -> { pending = profile; adding = false }
                                                else -> save(profile)
                                            }
                                        } catch (failure: Exception) {
                                            error = tr(R.string.ps_ui_30, failure.message.orEmpty())
                                        } finally { busy = false }
                                    }
                                }
                            }
                        }
                    }) {
                    Column(Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState()).padding(bottom = 12.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        val certificate = pending
                        val info = details
                        val deletion = removing
                        when {
                            certificate != null -> {
                                Text(certificate.url)
                                Text(tr(R.string.ps_ui_31))
                                Text(certificate.caSha256, style = MaterialTheme.typography.bodySmall)
                            }
                            info != null -> {
                                IPTextInput(value = editedNickname,
                                    onValueChange = { editedNickname = it.take(100) },
                                    label = { Text(tr(R.string.ps_ui_32)) },
                                    modifier = Modifier.fillMaxWidth().height(50.dp))
                                Text(info.url)
                                Text(tr(R.string.ps_ui_33))
                                Text(info.caSha256, style = MaterialTheme.typography.bodySmall)
                                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                                Row(Modifier.fillMaxWidth().padding(bottom = 12.dp),
                                    horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                    IPButton(text = tr(R.string.ps_ui_34),
                                        modifier = Modifier.weight(1f).height(44.dp),
                                        onClick = { removeServer = false; deleteAccount = true; details = null; removing = info })
                                    IPButton(text = tr(R.string.ps_ui_16),
                                        modifier = Modifier.weight(1f).height(44.dp),
                                        onClick = { removeServer = true; deleteAccount = false; details = null; removing = info })
                                }
                            }
                            deletion != null -> {
                                Text(deletion.url)
                                if (removeServer) {
                                    Text(if (selected == deletion.id)
                                        tr(R.string.ps_ui_35)
                                        else tr(R.string.ps_ui_36))
                                    IPSwitch(text = tr(R.string.ps_ui_37), checked = deleteAccount,
                                        onCheckedChange = { deleteAccount = it })
                                } else {
                                    Text(tr(R.string.ps_ui_38))
                                }
                                Text(if (deleteAccount)
                                    tr(R.string.ps_ui_39)
                                    else tr(R.string.ps_ui_40))
                            }
                            else -> {
                                if (!busy) IPTextInput(value = nickname,
                                    onValueChange = { nickname = it.take(100) },
                                    label = { Text(tr(R.string.ps_ui_41)) },
                                    modifier = Modifier.fillMaxWidth().height(50.dp))
                                if (!busy) IPTextInput(value = address,
                                    onValueChange = { address = it; error = null },
                                    label = { Text(tr(R.string.ps_ui_42)) },
                                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                                    modifier = Modifier.fillMaxWidth().height(50.dp))
                                else Text(tr(R.string.ps_ui_43))
                                Text(tr(R.string.ps_ui_44),
                                    style = MaterialTheme.typography.bodySmall)
                                Text(tr(R.string.ps_ui_45),
                                    style = MaterialTheme.typography.bodySmall)
                                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                            }
                        }
                    }
                }
            }
        }
    }
}
