package com.desky.client

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.desky.client.ui.DeskyNavHost
import com.desky.client.ui.theme.DeskyTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            DeskyTheme {
                DeskyNavHost()
            }
        }
    }
}
