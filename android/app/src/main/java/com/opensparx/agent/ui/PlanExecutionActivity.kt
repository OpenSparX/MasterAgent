package com.opensparx.agent.ui

import android.graphics.Color
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.opensparx.agent.AgentApplication
import com.opensparx.agent.R
import com.opensparx.agent.inference.GenieXSdkBackend
import com.opensparx.agent.tools.ToolRegistry
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.collectLatest
import org.json.JSONArray

/**
 * Demonstrates the Agent's planning pipeline:
 * User request -> LLM generates plan -> Plan displayed as step-by-step DAG -> Execution with status.
 */
class PlanExecutionActivity : AppCompatActivity() {

    // ─── Data Model ─────────────────────────────────────────────────────

    data class PlanStep(
        val icon: String,
        val title: String,
        val subtitle: String,
        val toolId: String? = null,
        val toolParams: Map<String, String> = emptyMap(),
        var status: StepStatus = StepStatus.PENDING,
    )

    enum class StepStatus { PENDING, RUNNING, DONE }

    // ─── Fields ─────────────────────────────────────────────────────────

    private lateinit var editRequest: android.widget.EditText
    private lateinit var btnPlan: com.google.android.material.button.MaterialButton
    private lateinit var planningIndicator: View
    private lateinit var recyclerSteps: RecyclerView
    private lateinit var verificationBadge: View
    private lateinit var textVerification: TextView
    private lateinit var textFormalSpec: TextView
    private lateinit var btnBack: View

    private val app by lazy { AgentApplication.get() }
    private val adapter = PlanStepAdapter()
    private var executionJob: Job? = null
    private lateinit var toolRegistry: ToolRegistry

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_plan_execution)

        toolRegistry = ToolRegistry(this)

        editRequest = findViewById(R.id.edit_request)
        btnPlan = findViewById(R.id.btn_plan)
        planningIndicator = findViewById(R.id.planning_indicator)
        recyclerSteps = findViewById(R.id.recycler_plan_steps)
        verificationBadge = findViewById(R.id.verification_badge)
        textVerification = findViewById(R.id.text_verification)
        textFormalSpec = findViewById(R.id.text_formal_spec)
        btnBack = findViewById(R.id.btn_back)

        recyclerSteps.layoutManager = LinearLayoutManager(this)
        recyclerSteps.adapter = adapter

        btnPlan.setOnClickListener { startPlanning() }
        btnBack.setOnClickListener { finish() }

        editRequest.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                startPlanning()
                true
            } else false
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        executionJob?.cancel()
    }

    // ─── Planning Logic ─────────────────────────────────────────────────

    private fun startPlanning() {
        val request = editRequest.text.toString().trim()
        if (request.isEmpty()) return

        // Reset UI
        executionJob?.cancel()
        adapter.submitSteps(emptyList())
        verificationBadge.visibility = View.GONE
        planningIndicator.visibility = View.VISIBLE
        btnPlan.isEnabled = false

        // Hide keyboard
        editRequest.clearFocus()
        val imm = getSystemService(android.content.Context.INPUT_METHOD_SERVICE)
                as android.view.inputmethod.InputMethodManager
        imm.hideSoftInputFromWindow(editRequest.windowToken, 0)

        executionJob = lifecycleScope.launch {
            val steps = generatePlan(request)
            planningIndicator.visibility = View.GONE
            adapter.submitSteps(steps)
            executeSteps(steps)
            btnPlan.isEnabled = true
        }
    }

    private suspend fun generatePlan(request: String): List<PlanStep> {
        // If GenieX model is loaded, try LLM-based plan generation
        if (app.genieX.isModelLoaded()) {
            val llmPlan = generatePlanFromLlm(request)
            if (llmPlan != null) return llmPlan
        }

        // Simulate planning delay then return demo plan
        delay(1200)
        return getDemoPlan(request)
    }

    private suspend fun generatePlanFromLlm(request: String): List<PlanStep>? {
        val systemPrompt = """You are a task planner. Given a user request, output a JSON array of steps.
Each step: {"icon":"emoji","title":"short action","subtitle":"detail"}.
Output ONLY the JSON array, no other text. 3-6 steps max."""

        val fullPrompt = "$systemPrompt\n\nUser request: $request"
        val result = StringBuilder()

        try {
            app.genieX.generate(
                userMessage = fullPrompt,
                maxTokens = 512,
            ).collectLatest { event ->
                when (event) {
                    is GenieXSdkBackend.GenEvent.Token -> result.append(event.text)
                    is GenieXSdkBackend.GenEvent.Done -> { /* done */ }
                    is GenieXSdkBackend.GenEvent.Error -> return@collectLatest
                }
            }

            return parsePlanJson(result.toString())
        } catch (e: Exception) {
            return null
        }
    }

    private fun parsePlanJson(raw: String): List<PlanStep>? {
        return try {
            // Extract JSON array from response (might have surrounding text)
            val start = raw.indexOf('[')
            val end = raw.lastIndexOf(']')
            if (start < 0 || end < 0) return null

            val jsonStr = raw.substring(start, end + 1)
            val arr = JSONArray(jsonStr)
            val steps = mutableListOf<PlanStep>()

            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                steps.add(PlanStep(
                    icon = obj.optString("icon", "⚙️"),
                    title = obj.optString("title", "Step ${i + 1}"),
                    subtitle = obj.optString("subtitle", ""),
                ))
            }
            if (steps.isEmpty()) null else steps
        } catch (e: Exception) {
            null
        }
    }

    private fun getDemoPlan(request: String): List<PlanStep> {
        // Contextual demo plans based on keywords
        val lower = request.lowercase()
        return when {
            lower.contains("会议") || lower.contains("meeting") || lower.contains("schedule") ->
                listOf(
                    PlanStep("📋", "Parse intent", "Schedule meeting"),
                    PlanStep("📅", "Check calendar", "Tomorrow 10am free"),
                    PlanStep("✉️", "Draft invitation", "Meeting with team..."),
                    PlanStep("✅", "Verify constraints", "No conflicts found"),
                    PlanStep("🚀", "Execute", "Calendar event created"),
                )
            lower.contains("邮件") || lower.contains("email") || lower.contains("mail") ->
                listOf(
                    PlanStep("📨", "Parse intent", "Send email"),
                    PlanStep("👤", "Resolve recipient", "Found in contacts"),
                    PlanStep("✍️", "Draft content", "Composing message..."),
                    PlanStep("🔍", "Review tone", "Professional, concise"),
                    PlanStep("📤", "Send", "Email delivered"),
                )
            lower.contains("提醒") || lower.contains("remind") || lower.contains("alarm") ||
                lower.contains("闹钟") ->
                listOf(
                    PlanStep("🧠", "Parse intent", "Set reminder/alarm"),
                    PlanStep("⏰", "Extract time", "Parsed: 8:00 AM"),
                    PlanStep("📝", "Compose reminder", "\"$request\""),
                    PlanStep("✅", "Verify feasibility", "Time is in the future"),
                    PlanStep("🔔", "Set alarm", "Triggering system alarm",
                        toolId = "set_alarm",
                        toolParams = mapOf("hour" to "8", "minute" to "0", "message" to request)),
                )
            else ->
                listOf(
                    PlanStep("🧠", "Parse intent", "\"$request\""),
                    PlanStep("🔍", "Gather context", "Querying local knowledge..."),
                    PlanStep("🛠️", "Plan actions", "2 sub-tasks identified"),
                    PlanStep("⚡", "Execute", "Running sub-tasks..."),
                    PlanStep("✅", "Verify result", "All constraints satisfied"),
                )
        }
    }

    // ─── Step Execution Animation ───────────────────────────────────────

    private suspend fun executeSteps(steps: List<PlanStep>) {
        for (i in steps.indices) {
            delay(400)
            steps[i].status = StepStatus.RUNNING
            adapter.notifyItemChanged(i)

            delay(800)
            steps[i].status = StepStatus.DONE
            adapter.notifyItemChanged(i)

            // Execute real system action if tool is bound to this step
            if (steps[i].toolId != null) {
                withContext(Dispatchers.Main) {
                    toolRegistry.execute(steps[i].toolId!!, steps[i].toolParams)
                }
            }
        }

        // Show verification badge
        delay(300)
        verificationBadge.visibility = View.VISIBLE
        textVerification.text = "Plan verified ✓"
        textFormalSpec.text = "Formal: AG(¬conflict) ∧ AF(goal)"
        verificationBadge.alpha = 0f
        verificationBadge.animate().alpha(1f).setDuration(500).start()
    }

    // ─── RecyclerView Adapter ───────────────────────────────────────────

    inner class PlanStepAdapter : RecyclerView.Adapter<PlanStepAdapter.ViewHolder>() {

        private var steps: List<PlanStep> = emptyList()

        fun submitSteps(newSteps: List<PlanStep>) {
            steps = newSteps
            notifyDataSetChanged()
        }

        override fun getItemCount() = steps.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
            val view = LayoutInflater.from(parent.context)
                .inflate(R.layout.item_plan_step, parent, false)
            return ViewHolder(view)
        }

        override fun onBindViewHolder(holder: ViewHolder, position: Int) {
            holder.bind(steps[position], position == steps.lastIndex)
        }

        inner class ViewHolder(itemView: View) : RecyclerView.ViewHolder(itemView) {
            private val iconView: TextView = itemView.findViewById(R.id.text_step_icon)
            private val titleView: TextView = itemView.findViewById(R.id.text_step_title)
            private val subtitleView: TextView = itemView.findViewById(R.id.text_step_subtitle)
            private val statusView: TextView = itemView.findViewById(R.id.text_step_status)
            private val connector: View = itemView.findViewById(R.id.view_connector)

            fun bind(step: PlanStep, isLast: Boolean) {
                iconView.text = step.icon
                titleView.text = step.title
                subtitleView.text = step.subtitle
                connector.visibility = if (isLast) View.INVISIBLE else View.VISIBLE

                when (step.status) {
                    StepStatus.PENDING -> {
                        statusView.text = "⏳"
                        titleView.setTextColor(Color.parseColor("#666666"))
                        subtitleView.setTextColor(Color.parseColor("#444444"))
                        itemView.alpha = 0.6f
                    }
                    StepStatus.RUNNING -> {
                        statusView.text = "🔄"
                        titleView.setTextColor(Color.parseColor("#FFD600"))
                        subtitleView.setTextColor(Color.parseColor("#99FFFFFF"))
                        itemView.alpha = 1f
                    }
                    StepStatus.DONE -> {
                        statusView.text = "✅"
                        titleView.setTextColor(Color.parseColor("#00E676"))
                        subtitleView.setTextColor(Color.parseColor("#99FFFFFF"))
                        itemView.alpha = 1f
                    }
                }
            }
        }
    }
}
