package io.github.cylear.hoshimi.localify.ui.components

import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.compose.ui.tooling.preview.Preview

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp


@Composable
fun IPButton(
    onClick: () -> Unit,
    text: String,
    modifier: Modifier = Modifier,
    shape: Shape = RoundedCornerShape(50.dp), // Rounded corners on left and right
    shadowElevation: Dp = 8.dp, // Shadow elevation
    borderWidth: Dp = 1.dp, // Border width
    borderColor: Color = Color.Transparent, // Border color
    enabled: Boolean = true,
    bgColors: List<Color>? = null,
    textColor: Color? = null
) {
    var buttonSize by remember { mutableStateOf(IntSize.Zero) }

    val gradient = remember(buttonSize, enabled, bgColors) {
        Brush.linearGradient(
            colors = bgColors
                ?: if (enabled) listOf(Color(0xFF1428FF), Color(0xFF5335FA)) else
                    listOf(Color(0xFFF9F9F9), Color(0xFFF0F0F0)),
            start = Offset(0f, 0f),
            end = Offset(buttonSize.width.toFloat(), buttonSize.height.toFloat()) // Dynamic end point
        )
    }

    Button(
        onClick = onClick,
        enabled = enabled,
        colors = ButtonDefaults.buttonColors(
            containerColor = Color.Transparent
        ),
        modifier = modifier
            .onGloballyPositioned { layoutCoordinates ->
                buttonSize = layoutCoordinates.size
            }
            .shadow(elevation = shadowElevation, shape = shape)
            .clip(shape)
            .background(gradient)
            .border(borderWidth, borderColor, shape),
        contentPadding = PaddingValues(0.dp)
    ) {
        Text(text = text, color = textColor ?: if (enabled) Color.White else Color(0xFF111111))
    }
}


@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO)
@Composable
fun IPButtonPreview() {
    IPButton(modifier = Modifier.width(80.dp).height(40.dp), text = "Button", onClick = {},
        enabled = true)
}
