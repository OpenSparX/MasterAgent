package com.opensparx.agent.tools

import android.content.Context
import android.content.Intent
import android.provider.AlarmClock
import android.net.Uri
import android.util.Log

/**
 * Tool Registry — system capabilities the Agent can invoke.
 *
 * Each tool maps to an Android system action:
 * - set_alarm → AlarmClock intent
 * - open_url → Browser intent
 * - set_timer → Timer intent
 * - search → Web search intent
 * - send_message → SMS/messaging intent
 * - take_photo → Camera intent
 */
class ToolRegistry(private val context: Context) {

    companion object {
        private const val TAG = "ToolRegistry"
    }

    data class Tool(
        val id: String,
        val name: String,
        val icon: String,
        val description: String,
    )

    val availableTools = listOf(
        Tool("set_alarm", "Set Alarm", "⏰", "Create an alarm at specified time"),
        Tool("open_url", "Open URL", "🌐", "Open a webpage in browser"),
        Tool("set_timer", "Set Timer", "⏱️", "Start a countdown timer"),
        Tool("search", "Web Search", "🔍", "Search the web"),
        Tool("take_photo", "Camera", "📷", "Open camera to take a photo"),
        Tool("flashlight", "Flashlight", "🔦", "Toggle flashlight"),
    )

    /**
     * Execute a tool by ID. Returns true if launched successfully.
     */
    fun execute(toolId: String, params: Map<String, String> = emptyMap()): Boolean {
        return try {
            when (toolId) {
                "set_alarm" -> {
                    val hour = params["hour"]?.toIntOrNull() ?: 8
                    val minute = params["minute"]?.toIntOrNull() ?: 0
                    val message = params["message"] ?: "Agent reminder"
                    val intent = Intent(AlarmClock.ACTION_SET_ALARM).apply {
                        putExtra(AlarmClock.EXTRA_HOUR, hour)
                        putExtra(AlarmClock.EXTRA_MINUTES, minute)
                        putExtra(AlarmClock.EXTRA_MESSAGE, message)
                        putExtra(AlarmClock.EXTRA_SKIP_UI, false)
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(intent)
                    true
                }
                "open_url" -> {
                    val url = params["url"] ?: "https://opensparx.com"
                    val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).apply {
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(intent)
                    true
                }
                "set_timer" -> {
                    val seconds = params["seconds"]?.toIntOrNull() ?: 60
                    val message = params["message"] ?: "Timer"
                    val intent = Intent(AlarmClock.ACTION_SET_TIMER).apply {
                        putExtra(AlarmClock.EXTRA_LENGTH, seconds)
                        putExtra(AlarmClock.EXTRA_MESSAGE, message)
                        putExtra(AlarmClock.EXTRA_SKIP_UI, false)
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(intent)
                    true
                }
                "search" -> {
                    val query = params["query"] ?: "OpenSparX Agent OS"
                    val intent = Intent(Intent.ACTION_WEB_SEARCH).apply {
                        putExtra("query", query)
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(intent)
                    true
                }
                "take_photo" -> {
                    val intent = Intent(android.provider.MediaStore.ACTION_IMAGE_CAPTURE).apply {
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(intent)
                    true
                }
                "flashlight" -> {
                    val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as android.hardware.camera2.CameraManager
                    val cameraId = cameraManager.cameraIdList[0]
                    cameraManager.setTorchMode(cameraId, params["on"]?.toBoolean() ?: true)
                    true
                }
                else -> {
                    Log.w(TAG, "Unknown tool: $toolId")
                    false
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Tool execution failed: $toolId", e)
            false
        }
    }
}
