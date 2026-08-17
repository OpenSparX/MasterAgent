package com.opensparx.agent.ui

import android.animation.ValueAnimator
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.os.IBinder
import android.view.Gravity
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.opensparx.agent.AgentApplication
import com.opensparx.agent.R
import kotlinx.coroutines.*

/**
 * Chat bubble overlay — expands from the floating pet when tapped.
 *
 * Shows a minimal chat interface:
 * - Streaming token-by-token response
 * - Current backend indicator (GenieX / NeuroPilot / CPU)
 * - Speed display (tok/s)
 * - Proactive suggestion cards
 *
 * Design: Material 3 dark, rounded corners, semi-transparent background.
 * Dismisses on outside tap or swipe down.
 */
class ChatBubbleService : Service() {

    companion object {
        const val EXTRA_ANCHOR_Y = "anchor_y"
        const val ACTION_DISMISS = "com.opensparx.agent.DISMISS_CHAT"
        const val ACTION_PROACTIVE_SUGGESTION = "com.opensparx.agent.PROACTIVE_SUGGEST"
        const val EXTRA_SUGGESTION = "suggestion"
    }

    private lateinit var windowManager: WindowManager
    private var bubbleView: View? = null
    private val serviceScope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private val app by lazy { AgentApplication.get() }

    private var generationJob: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_DISMISS -> dismiss()
            ACTION_PROACTIVE_SUGGESTION -> {
                val suggestion = intent.getStringExtra(EXTRA_SUGGESTION) ?: return START_NOT_STICKY
                showWithSuggestion(suggestion)
            }
            else -> show(intent?.getIntExtra(EXTRA_ANCHOR_Y, 300) ?: 300)
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        dismiss()
        serviceScope.cancel()
        super.onDestroy()
    }

    private fun show(anchorY: Int) {
        if (bubbleView != null) return // already showing

        bubbleView = LayoutInflater.from(this).inflate(R.layout.chat_bubble, null)

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = anchorY + 80  // below the pet widget
            horizontalMargin = 0.03f
        }

        setupChatUI()
        windowManager.addView(bubbleView, params)
        animateIn()
    }

    private fun showWithSuggestion(suggestion: String) {
        show(300)
        // Pre-fill with proactive suggestion
        bubbleView?.let { view ->
            val messagesContainer = view.findViewById<LinearLayout>(R.id.messages_container)
            addSystemMessage(messagesContainer, "💡 Suggestion based on context:")
            addSystemMessage(messagesContainer, suggestion)

            // Add accept/dismiss buttons
            val input = view.findViewById<EditText>(R.id.input_field)
            input.hint = "Ask follow-up or tap ✓ to accept..."
        }
    }

    private fun setupChatUI() {
        val view = bubbleView ?: return
        val input = view.findViewById<EditText>(R.id.input_field)
        val sendBtn = view.findViewById<ImageButton>(R.id.btn_send)
        val closeBtn = view.findViewById<ImageButton>(R.id.btn_close)
        val backendLabel = view.findViewById<TextView>(R.id.text_backend)

        // Show which backend is active
        backendLabel.text = app.backend.name

        // Send on button click
        sendBtn.setOnClickListener {
            val text = input.text.toString().trim()
            if (text.isNotEmpty()) {
                sendMessage(text)
                input.text.clear()
            }
        }

        // Send on Enter key
        input.setOnEditorActionListener { _, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_SEND ||
                (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)) {
                sendBtn.performClick()
                true
            } else false
        }

        // Close bubble
        closeBtn.setOnClickListener { dismiss() }
    }

    private fun sendMessage(text: String) {
        val view = bubbleView ?: return
        val messagesContainer = view.findViewById<LinearLayout>(R.id.messages_container)
        val scrollView = view.findViewById<ScrollView>(R.id.scroll_messages)
        val speedLabel = view.findViewById<TextView>(R.id.text_speed)

        // Add user message
        addUserMessage(messagesContainer, text)
        scrollView.post { scrollView.fullScroll(View.FOCUS_DOWN) }

        // Cancel any existing generation
        generationJob?.cancel()
        app.backend.cancelGeneration()

        // Start streaming response
        val responseView = addAssistantMessage(messagesContainer, "")
        scrollView.post { scrollView.fullScroll(View.FOCUS_DOWN) }

        generationJob = serviceScope.launch {
            try {
                val result = app.backend.generate(
                    prompt = buildPrompt(text),
                    onToken = { token ->
                        launch(Dispatchers.Main) {
                            responseView.append(token)
                            scrollView.post { scrollView.fullScroll(View.FOCUS_DOWN) }
                        }
                    }
                )
                withContext(Dispatchers.Main) {
                    speedLabel.text = String.format("%.1f tok/s", result.tokensPerSecond)
                }
            } catch (e: CancellationException) {
                // Normal cancellation
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    responseView.text = "Error: ${e.message}"
                }
            }
        }
    }

    private fun buildPrompt(userMessage: String): String {
        // Simple chat template — will be expanded with conversation history
        return """<|im_start|>system
You are SparX, a helpful on-device AI assistant running on the user's phone.
Be concise — you're in a floating chat bubble. Keep responses under 3 sentences unless asked for more.
<|im_end|>
<|im_start|>user
$userMessage
<|im_end|>
<|im_start|>assistant
"""
    }

    private fun addUserMessage(container: LinearLayout, text: String) {
        val tv = TextView(this).apply {
            this.text = text
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundResource(R.drawable.chat_bubble_user)
            setPadding(24, 16, 24, 16)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.END
                setMargins(64, 8, 8, 8)
            }
            layoutParams = lp
        }
        container.addView(tv)
    }

    private fun addAssistantMessage(container: LinearLayout, text: String): TextView {
        val tv = TextView(this).apply {
            this.text = text
            setTextColor(0xFFE0E0E0.toInt())
            setBackgroundResource(R.drawable.chat_bubble_assistant)
            setPadding(24, 16, 24, 16)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.START
                setMargins(8, 8, 64, 8)
            }
            layoutParams = lp
        }
        container.addView(tv)
        return tv
    }

    private fun addSystemMessage(container: LinearLayout, text: String) {
        val tv = TextView(this).apply {
            this.text = text
            setTextColor(0xFF00FF88.toInt())
            textSize = 13f
            setPadding(24, 8, 24, 8)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                setMargins(8, 4, 8, 4)
            }
            layoutParams = lp
        }
        container.addView(tv)
    }

    private fun animateIn() {
        bubbleView?.let { view ->
            view.alpha = 0f
            view.translationY = -30f
            view.animate()
                .alpha(1f)
                .translationY(0f)
                .setDuration(200)
                .setInterpolator(DecelerateInterpolator())
                .start()
        }
    }

    private fun dismiss() {
        generationJob?.cancel()
        bubbleView?.let { view ->
            view.animate()
                .alpha(0f)
                .translationY(-20f)
                .setDuration(150)
                .withEndAction {
                    try { windowManager.removeView(view) } catch (_: Exception) {}
                    bubbleView = null
                }
                .start()
        }
        stopSelf()
    }
}
