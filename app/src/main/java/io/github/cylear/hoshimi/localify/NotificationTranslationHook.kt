package io.github.cylear.hoshimi.localify

import android.app.AndroidAppHelper
import android.app.Application
import android.app.Notification
import android.app.NotificationManager
import android.content.Context
import android.util.Log
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge

object NotificationTranslationHook {
    private val callDepth = ThreadLocal<Int>()

    fun install(packageName: String) {
        XposedBridge.hookAllMethods(NotificationManager::class.java, "setNotificationDelegate", object : XC_MethodHook() {
            override fun beforeHookedMethod(param: MethodHookParam) {
                val context = AndroidAppHelper.currentApplication()?.applicationContext
                if (context == null || NotificationLocalizer.isEnabled(context)) param.args[0] = null
            }
        })
        XposedBridge.hookAllMethods(Application::class.java, "attach", object : XC_MethodHook() {
            override fun afterHookedMethod(param: MethodHookParam) {
                val context = param.args.firstOrNull() as? Context ?: return
                if (!NotificationLocalizer.isEnabled(context)) return
                try {
                    val manager = context.getSystemService(NotificationManager::class.java)
                    if (manager?.notificationDelegate != null) manager.setNotificationDelegate(null)
                } catch (error: Exception) {
                    Log.w(TAG, "Could not disable notification delegation.", error)
                }
            }
        })
        val hook = object : XC_MethodHook() {
            override fun beforeHookedMethod(param: MethodHookParam) {
                val depth = callDepth.get() ?: 0
                callDepth.set(depth + 1)
                if (depth != 0) return
                if (param.method.name == "notifyAsPackage" && param.args.firstOrNull() != packageName) return
                val index = param.args.indexOfFirst { it is Notification }
                if (index < 0) return
                val context = AndroidAppHelper.currentApplication()?.applicationContext ?: return
                try {
                    param.args[index] = NotificationLocalizer.localize(context, param.args[index] as Notification)
                } catch (error: Exception) {
                    Log.w(TAG, "Notification translation failed; keeping the original notification.", error)
                } catch (error: LinkageError) {
                    Log.w(TAG, "Notification translator unavailable; keeping the original notification.", error)
                }
            }

            override fun afterHookedMethod(param: MethodHookParam) {
                val depth = callDepth.get() ?: 1
                if (depth <= 1) callDepth.remove() else callDepth.set(depth - 1)
            }
        }
        XposedBridge.hookAllMethods(NotificationManager::class.java, "notify", hook)
        XposedBridge.hookAllMethods(NotificationManager::class.java, "notifyAsPackage", hook)
    }
}
