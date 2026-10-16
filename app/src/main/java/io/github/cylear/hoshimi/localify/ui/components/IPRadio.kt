package io.github.cylear.hoshimi.localify.ui.components

import android.content.res.Configuration.UI_MODE_NIGHT_NO
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalMinimumInteractiveComponentEnforcement
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.cylear.hoshimi.localify.ui.components.base.AutoSizeText

@Composable
@OptIn(ExperimentalMaterial3Api::class)
fun IPRadio(
    modifier: Modifier = Modifier,
    text: String,
    selected: Boolean,
    fontSize: TextUnit = 14.sp,
    compact: Boolean = false,
    onClick: () -> Unit
) {
    val backgroundColor = if (selected) Color(0xFFF1F1EB) else Color(0xFFF8F7F5)
    val radioButtonColor = if (selected) Color(0xFF1428FF) else MaterialTheme.colorScheme.onSurface

    Surface(
        shape = RoundedCornerShape(
            topStart = 4.dp,
            topEnd = 16.dp,
            bottomEnd = 4.dp,
            bottomStart = 16.dp
        ),
        color = backgroundColor,
        modifier = modifier
            .pointerInput(Unit) {
                detectTapGestures(onTap = {
                    onClick()
                })
            }
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = modifier.padding(start = 0.dp, end = 4.dp)
        ) {
            CompositionLocalProvider(LocalMinimumInteractiveComponentEnforcement provides
                (!compact && LocalMinimumInteractiveComponentEnforcement.current)) {
                RadioButton(
                    modifier = if (compact) Modifier.size(28.dp) else Modifier.padding(start = 0.dp),
                    selected = selected,
                    onClick = onClick,
                    colors = RadioButtonDefaults.colors(
                        selectedColor = radioButtonColor,
                        unselectedColor = MaterialTheme.colorScheme.onSurface
                    )
                )
            }
            // Spacer(modifier = modifier.width(16.dp))
            AutoSizeText(text = text,
                textStyle = TextStyle(color = MaterialTheme.colorScheme.onSurface,
                    fontSize = fontSize))
            // Text(text = text, color = MaterialTheme.colorScheme.onSurface, fontSize = fontSize)
        }
    }
}



@Preview(showBackground = true, uiMode = UI_MODE_NIGHT_NO, widthDp = 100, heightDp = 40)
@Composable
fun IPRadioPreview() {
    IPRadio(text = "IPRadioooo", selected = true, onClick = {})
}
