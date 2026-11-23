package io.github.cylear.hoshimi.localify.ui.game_attach

import android.app.Activity
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Path
import android.graphics.Typeface
import android.graphics.drawable.ClipDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.InsetDrawable
import android.graphics.drawable.LayerDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.ShapeDrawable
import android.graphics.drawable.shapes.PathShape
import android.os.Build
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import io.github.cylear.hoshimi.localify.HoshimiHookMain
import kotlin.math.roundToInt

private const val TICKS_PER_SECOND = 20

internal class LiveSeekOverlay {
    private var panel: LinearLayout? = null
    private var slider: SeekBar? = null
    private var label: TextView? = null
    private var playback: ImageButton? = null
    private var paused: Boolean? = null
    private var session = 0
    private var total = 0
    private var dragging = false
    private var cancelled = false

    fun render(activity: Activity, state: Long) {
        val host = activity.findViewById<FrameLayout>(android.R.id.content)
        val nextSession = (state ushr 42).toInt()
        if (session != nextSession || panel?.parent !== host) detach()
        if (state == 0L || host == null || activity.isDestroyed || activity.isFinishing) {
            detach()
            return
        }
        session = nextSession
        total = ((state ushr 20) and 0xfffff).toInt()
        val isPanorama = (state and (1L shl 41)) != 0L
        if (panel == null) attach(activity, host, isPanorama)
        val isPaused = (state and (1L shl 40)) != 0L
        if (paused != isPaused) {
            paused = isPaused
            val iconSize = (14 * activity.resources.displayMetrics.density).roundToInt()
            playback?.setImageDrawable(playbackIcon(isPaused, iconSize))
            val korean = activity.resources.configuration.locales[0].language == "ko"
            playback?.contentDescription = if (isPaused) {
                if (korean) "재생" else "Play"
            } else {
                if (korean) "일시정지" else "Pause"
            }
        }
        if (!dragging && total > 0) {
            val position = (state and 0xfffff).toInt()
            slider?.progress = (1000f * position / total).roundToInt()
            showTime(position)
        }
    }

    private fun detach() {
        session = 0
        dragging = false
        panel?.let { (it.parent as? ViewGroup)?.removeView(it) }
        panel = null
        slider = null
        label = null
        playback = null
        paused = null
    }

    private fun showTime(position: Int) {
        fun clock(ticks: Int): String {
            val seconds = ticks / TICKS_PER_SECOND
            val centiseconds = ticks % TICKS_PER_SECOND * 100 / TICKS_PER_SECOND
            return "${seconds / 60}:${(seconds % 60).toString().padStart(2, '0')}.${centiseconds.toString().padStart(2, '0')}"
        }
        label?.text = "${clock(position)} / ${clock(total)}"
    }

    private fun playbackIcon(play: Boolean, size: Int): ShapeDrawable {
        val path = Path().apply {
            if (play) {
                moveTo(7f, 3f)
                lineTo(21f, 12f)
                lineTo(7f, 21f)
                close()
            } else {
                addRoundRect(5f, 4f, 10f, 20f, 1f, 1f, Path.Direction.CW)
                addRoundRect(14f, 4f, 19f, 20f, 1f, 1f, Path.Direction.CW)
            }
        }
        return ShapeDrawable(PathShape(path, 24f, 24f)).apply {
            intrinsicWidth = size
            intrinsicHeight = size
            paint.color = Color.WHITE
            paint.isAntiAlias = true
        }
    }

    private fun attach(activity: Activity, host: FrameLayout, top: Boolean) {
        fun dp(size: Int) = (size * activity.resources.displayMetrics.density).roundToInt()
        fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
            cornerRadius = dp(radius).toFloat()
            setColor(color)
        }
        val blue = 0xff2934f5.toInt()
        val violet = 0xff6540fa.toInt()
        fun brandFill() = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, intArrayOf(blue, violet)).apply {
            cornerRadius = dp(20).toFloat()
        }
        fun keyBackground(primary: Boolean): InsetDrawable {
            val fill = if (primary) brandFill() else rounded(Color.WHITE, 18)
            fill.setStroke(dp(1), if (primary) 0xffadb0ff.toInt() else 0xffc7c9ee.toInt())
            val ripple = RippleDrawable(ColorStateList.valueOf(if (primary) 0x55ffffff else 0x33343aff), fill, null)
            return InsetDrawable(ripple, dp(2), dp(2), dp(2), dp(2))
        }
        val bar = SeekBar(activity).apply {
            max = 1000
            progressDrawable = LayerDrawable(arrayOf(
                rounded(0xffc4c6d5.toInt(), 2),
                ClipDrawable(brandFill(), Gravity.LEFT, ClipDrawable.HORIZONTAL)
            )).apply {
                setId(0, android.R.id.background)
                setId(1, android.R.id.progress)
                for (index in 0 until numberOfLayers) {
                    setLayerHeight(index, dp(3))
                    setLayerGravity(index, Gravity.FILL_HORIZONTAL or Gravity.CENTER_VERTICAL)
                }
            }
            progressTintList = null
            progressBackgroundTintList = null
            thumb = rounded(Color.WHITE, 8).apply {
                setSize(dp(14), dp(14))
                setStroke(dp(3), blue)
            }
            thumbTintList = null
            splitTrack = false
            background = null
            layoutDirection = View.LAYOUT_DIRECTION_LTR
            setPadding(dp(8), 0, dp(8), 0)
            contentDescription = if (activity.resources.configuration.locales[0].language == "ko") "재생 위치" else "Playback position"
        }
        label = TextView(activity).apply {
            setTextColor(0xff242535.toInt())
            textSize = 11f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            fontFeatureSettings = "tnum"
            setSingleLine()
        }
        bar.setOnTouchListener { _, event ->
            cancelled = event.actionMasked == MotionEvent.ACTION_CANCEL
            false
        }
        bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(seekBar: SeekBar) { dragging = true }
            override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
                if (fromUser && slider === seekBar) {
                    showTime(total * progress / 1000)
                    if (!cancelled) HoshimiHookMain.requestLiveSeek(session, progress / 1000f)
                }
            }
            override fun onStopTrackingTouch(seekBar: SeekBar) {
                if (!cancelled && slider === seekBar) HoshimiHookMain.requestLiveSeek(session, seekBar.progress / 1000f)
                dragging = false
            }
        })
        playback = ImageButton(activity).apply {
            stateListAnimator = null
            backgroundTintList = null
            background = keyBackground(true)
            imageTintList = null
            scaleType = ImageView.ScaleType.CENTER_INSIDE
            setPadding(dp(6), dp(6), dp(6), dp(6))
            setOnClickListener { paused?.let { HoshimiHookMain.setLivePaused(session, !it) } }
        }
        slider = bar
        val controls = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(6), dp(1), dp(6), 0)
            background = rounded(0xb8fafaff.toInt(), 16).apply { setStroke(dp(1), 0x88ffffff.toInt()) }
            elevation = dp(2).toFloat()
        }
        val stepRow = LinearLayout(activity).apply { gravity = Gravity.CENTER_VERTICAL }
        val buttonHeight = dp(30)
        stepRow.addView(playback, LinearLayout.LayoutParams(dp(32), buttonHeight))
        for (direction in listOf(-1, 1)) {
            val button = Button(activity).apply {
                text = if (direction < 0) "−0.05" else "+0.05"
                isAllCaps = false
                backgroundTintList = null
                background = keyBackground(false)
                stateListAnimator = null
                setPadding(dp(4), 0, dp(4), 0)
                minimumWidth = 0
                minWidth = 0
                minHeight = 0
                textSize = 11f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                setTextColor(blue)
                setSingleLine()
                setOnClickListener { HoshimiHookMain.stepLiveSeek(session, direction) }
            }
            stepRow.addView(button, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, buttonHeight))
        }
        label?.gravity = Gravity.END or Gravity.CENTER_VERTICAL
        label?.setPadding(0, 0, dp(6), 0)
        stepRow.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        controls.addView(stepRow)
        controls.addView(bar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(28)))
        panel = controls
        fun width(): Int {
            val maxW = dp(360)
            val minW = if (top) dp(230) else dp(250)
            val ratio = if (top) 0.62f else 0.68f
            return minOf(maxW, maxOf(minW, (host.width * ratio).roundToInt()), (host.width - dp(16)).coerceAtLeast(0))
        }
        val edgeMargin = dp(34)
        fun topOffset(): Int {
            val safeTop = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                host.rootWindowInsets?.displayCutout?.safeInsetTop ?: 0
            } else {
                0
            }
            return safeTop + edgeMargin
        }
        val gravity = (if (top) Gravity.TOP else Gravity.BOTTOM) or Gravity.CENTER_HORIZONTAL
        host.addView(controls, FrameLayout.LayoutParams(width(), ViewGroup.LayoutParams.WRAP_CONTENT, gravity).apply {
            if (top) topMargin = topOffset() else bottomMargin = edgeMargin
        })
        val resize = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            val lp = controls.layoutParams as FrameLayout.LayoutParams
            val targetWidth = width()
            val targetTop = if (top) topOffset() else lp.topMargin
            if (lp.width != targetWidth || lp.topMargin != targetTop) {
                lp.width = targetWidth
                lp.topMargin = targetTop
                controls.layoutParams = lp
            }
        }
        host.addOnLayoutChangeListener(resize)
        controls.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(view: View) = Unit
            override fun onViewDetachedFromWindow(view: View) { host.removeOnLayoutChangeListener(resize) }
        })
    }
}
