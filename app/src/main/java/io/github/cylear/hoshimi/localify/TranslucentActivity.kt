package io.github.cylear.hoshimi.localify

import android.os.Bundle
import androidx.activity.ComponentActivity
import io.github.cylear.hoshimi.localify.models.IdolyprideConfig
import io.github.cylear.hoshimi.localify.models.ProgramConfig


class TranslucentActivity : ComponentActivity(), IConfigurableActivity<TranslucentActivity> {
    override lateinit var config: IdolyprideConfig
    override lateinit var programConfig: ProgramConfig

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        loadConfig()
        val requestData = intent.getStringExtra("iprData")
        if (requestData != null) {
            if (requestData == "requestConfig") {
                onClickStartGame(intent.getStringExtra(TargetGamePackages.EXTRA_REQUESTING_GAME_PACKAGE))
                finish()
            }
        }
    }
}
