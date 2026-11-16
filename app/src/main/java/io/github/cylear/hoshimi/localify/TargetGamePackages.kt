package io.github.cylear.hoshimi.localify

import android.content.Context
import android.content.pm.PackageManager

object TargetGamePackages {
    const val EXTRA_REQUESTING_GAME_PACKAGE = "requestingGamePackage"
    const val IDOLY_PRIDE = "game.qualiarts.idolypride"
    const val IDOLY_PRIDE_KR = "game.qualiarts.idolypride_kr"

    val all = listOf(
        IDOLY_PRIDE,
        IDOLY_PRIDE_KR,
    )

    fun findInstalled(context: Context, requestedPackage: String? = null): String? {
        val candidates = if (requestedPackage == null) {
            all
        } else {
            if (requestedPackage !in all) return null
            listOf(requestedPackage)
        }
        return candidates.firstOrNull {
            try {
                context.packageManager.getPackageInfo(it, 0)
                true
            } catch (_: PackageManager.NameNotFoundException) {
                false
            }
        }
    }
}
