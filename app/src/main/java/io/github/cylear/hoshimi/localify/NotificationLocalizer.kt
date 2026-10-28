package io.github.cylear.hoshimi.localify

import android.app.Notification
import android.app.Person
import android.content.Context
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import io.github.cylear.hoshimi.localify.hookUtils.FilesChecker
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import java.io.File

object NotificationLocalizer {
    private const val preferencesName = "hoshimi_local_notifications"
    private val textKeys = listOf(
        Notification.EXTRA_TITLE,
        Notification.EXTRA_TITLE_BIG,
        Notification.EXTRA_TEXT,
        Notification.EXTRA_BIG_TEXT,
        Notification.EXTRA_SUB_TEXT,
        Notification.EXTRA_SUMMARY_TEXT,
        Notification.EXTRA_INFO_TEXT,
        Notification.EXTRA_CONVERSATION_TITLE
    )

    private data class TextTarget(val original: CharSequence, val replace: (CharSequence) -> Unit)

    fun isEnabled(context: Context): Boolean {
        return context.getSharedPreferences(preferencesName, Context.MODE_PRIVATE).getBoolean("enabled", true)
    }

    fun updateConfig(context: Context, config: IdolyprideConfig) {
        context.getSharedPreferences(preferencesName, Context.MODE_PRIVATE).edit()
            .putBoolean("enabled", config.enabled)
            .putString("display_user_name", config.displayUserName)
            .apply()
    }

    @JvmStatic
    external fun translateTexts(basePath: String, userName: String, texts: Array<String>): Array<String>

    fun localize(context: Context, original: Notification): Notification {
        val preferences = context.getSharedPreferences(preferencesName, Context.MODE_PRIVATE)
        if (!preferences.getBoolean("enabled", true)) return original
        val notification = original.clone()
        val targets = mutableListOf<TextTarget>()
        collect(notification, targets)
        if (targets.isEmpty()) return original

        val texts = targets.map { it.original.toString() }.toTypedArray()
        val translated = translateTexts(
            File(context.filesDir, FilesChecker.localizationFilesDir).absolutePath,
            preferences.getString("display_user_name", "").orEmpty(),
            texts
        )
        check(translated.size == targets.size)
        var changed = false
        targets.forEachIndexed { index, target ->
            if (translated[index] != texts[index]) {
                val replacement = if (target.original is Spanned) {
                    SpannableStringBuilder(target.original).replace(0, target.original.length, translated[index])
                } else {
                    translated[index]
                }
                target.replace(replacement)
                changed = true
            }
        }
        if (!changed) return original
        notification.publicVersion?.let {
            notification.publicVersion = Notification.Builder.recoverBuilder(context, it).build()
        }
        return Notification.Builder.recoverBuilder(context, notification).build()
    }

    private fun collect(notification: Notification, targets: MutableList<TextTarget>) {
        notification.actions = notification.actions?.map { it.clone() }?.toTypedArray()
        notification.actions?.forEach { action ->
            action.title?.let { value ->
                targets.add(TextTarget(value) { action.title = it })
            }
        }
        notification.tickerText?.let { value ->
            targets.add(TextTarget(value) { notification.tickerText = it })
        }
        notification.extras = Bundle(notification.extras ?: Bundle())
        val extras = notification.extras
        for (key in textKeys) {
            extras.getCharSequence(key)?.let { value ->
                targets.add(TextTarget(value) { extras.putCharSequence(key, it) })
            }
        }
        extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)?.copyOf()?.let { lines ->
            lines.forEachIndexed { index, value ->
                if (value != null) {
                    targets.add(TextTarget(value) {
                        lines[index] = it
                        extras.putCharSequenceArray(Notification.EXTRA_TEXT_LINES, lines)
                    })
                }
            }
        }
        for (key in listOf(Notification.EXTRA_MESSAGES, Notification.EXTRA_HISTORIC_MESSAGES)) {
            @Suppress("DEPRECATION")
            val messages = extras.getParcelableArray(key)?.map { if (it is Bundle) Bundle(it) else it }?.toTypedArray()
                ?: continue
            for (message in messages) {
                if (message !is Bundle) continue
                for (field in listOf("text", "sender")) {
                    message.getCharSequence(field)?.let { value ->
                        targets.add(TextTarget(value) {
                            message.putCharSequence(field, it)
                            extras.putParcelableArray(key, messages)
                        })
                    }
                }
                @Suppress("DEPRECATION")
                val sender = message.getParcelable<Person>("sender_person")
                sender?.name?.let { value ->
                    targets.add(TextTarget(value) {
                        message.putParcelable("sender_person", sender.toBuilder().setName(it).build())
                        extras.putParcelableArray(key, messages)
                    })
                }
            }
        }
        @Suppress("DEPRECATION")
        val messagingUser = extras.getParcelable<Person>(Notification.EXTRA_MESSAGING_PERSON)
        messagingUser?.name?.let { value ->
            targets.add(TextTarget(value) {
                extras.putParcelable(Notification.EXTRA_MESSAGING_PERSON, messagingUser.toBuilder().setName(it).build())
            })
        }
        @Suppress("DEPRECATION")
        val people = extras.getParcelableArrayList<Person>(Notification.EXTRA_PEOPLE_LIST)?.let { ArrayList(it) }
        people?.forEachIndexed { index, person ->
            person.name?.let { value ->
                targets.add(TextTarget(value) {
                    people[index] = person.toBuilder().setName(it).build()
                    extras.putParcelableArrayList(Notification.EXTRA_PEOPLE_LIST, people)
                })
            }
        }
        notification.publicVersion?.let { collect(it, targets) }
    }
}
