package io.github.cylear.hoshimi.localify

import android.content.Context
import android.content.pm.PackageManager

object TargetGamePackages {
    const val IDOLY_PRIDE = "game.qualiarts.idolypride"
    const val IDOLY_PRIDE_KR = "game.qualiarts.idolypride_kr"

    val all = listOf(
        IDOLY_PRIDE,
        IDOLY_PRIDE_KR,
    )

    fun findInstalled(context: Context): String? {
        return all.firstOrNull {
            try {
                context.packageManager.getPackageInfo(it, 0)
                true
            } catch (_: PackageManager.NameNotFoundException) {
                false
            }
        }
    }
}
