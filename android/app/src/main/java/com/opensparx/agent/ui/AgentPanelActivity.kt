package com.opensparx.agent.ui

import android.os.Bundle
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import com.opensparx.agent.databinding.ActivityAgentPanelBinding
import com.opensparx.agent.jni.AgentBridge
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject

/**
 * AgentPanelActivity — full-screen visualization of the agent's thought process.
 *
 * Shows a DAG (directed acyclic graph) of agent nodes with animated edges
 * representing data flow. Nodes are progressively revealed as the plan
 * executes, giving users a transparent view into on-device reasoning.
 *
 * Opened by tapping the floating widget (FloatingAgentService).
 */
class AgentPanelActivity : AppCompatActivity() {

    private lateinit var binding: ActivityAgentPanelBinding

    private val activityScope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private var currentPlanId: Long = -1L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setupImmersive()

        binding = ActivityAgentPanelBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnClose.setOnClickListener { finish() }

        currentPlanId = intent.getLongExtra(EXTRA_PLAN_ID, -1L)
        loadPlanDag()
        startStatsPolling()
        startStatusPolling()
    }

    override fun onDestroy() {
        activityScope.cancel()
        super.onDestroy()
    }

    // ─── Immersive Setup ────────────────────────────────────────────────

    private fun setupImmersive() {
        window.setFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
        )
    }

    // ─── Plan Loading ───────────────────────────────────────────────────

    /**
     * Load the DAG structure from the native engine and build the graph.
     * If no plan ID was passed, fetch the latest active plan.
     */
    private fun loadPlanDag() {
        activityScope.launch(Dispatchers.IO) {
            try {
                if (currentPlanId < 0) {
                    // Find current active plan from engine state
                    val stateJson = AgentBridge.getAgentsState()
                    currentPlanId = parseActivePlanId(stateJson)
                }

                if (currentPlanId < 0) {
                    withContext(Dispatchers.Main) {
                        binding.planTitle.text = "No active plan"
                    }
                    return@launch
                }

                val dagJson = AgentBridge.getPlanDag(currentPlanId)
                val (nodes, edges) = parseDag(dagJson)

                withContext(Dispatchers.Main) {
                    binding.dagView.setGraph(nodes, edges)
                    binding.planTitle.text = "Plan #$currentPlanId"
                    // Progressive reveal — start with first node
                    startProgressiveReveal(nodes.size)
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    binding.planTitle.text = "Engine not ready"
                }
            }
        }
    }

    // ─── Progressive Reveal ────────────────────────────────────────────

    /**
     * Reveals nodes one at a time with a staggered delay,
     * simulating the plan "unfolding" in front of the user.
     */
    private fun startProgressiveReveal(totalNodes: Int) {
        activityScope.launch {
            for (i in 1..totalNodes) {
                delay(350L)
                binding.dagView.revealUpTo(i)
            }
        }
    }

    // ─── Stats Polling ──────────────────────────────────────────────────

    /** Poll NPU load, inference speed, and KV cache usage. */
    private fun startStatsPolling() {
        activityScope.launch {
            while (isActive) {
                try {
                    val npuLoad = AgentBridge.getNpuLoad()
                    val tokPerSec = AgentBridge.getInferenceSpeed()
                    val kvUsage = AgentBridge.getKvCacheUsage()

                    binding.statNpu.text = "NPU: ${(npuLoad * 100).toInt()}%"
                    binding.statToks.text = "tok/s: ${"%.1f".format(tokPerSec)}"
                    binding.statKv.text = "KV: ${(kvUsage * 100).toInt()}%"
                } catch (e: Exception) {
                    binding.statNpu.text = "NPU: --"
                    binding.statToks.text = "tok/s: --"
                    binding.statKv.text = "KV: --"
                }
                delay(500L)
            }
        }
    }

    // ─── Node Status Polling ────────────────────────────────────────────

    /** Poll plan status and update individual node states. */
    private fun startStatusPolling() {
        activityScope.launch {
            while (isActive) {
                try {
                    if (currentPlanId < 0) { delay(1000L); continue }
                    val statusJson = AgentBridge.getPlanStatus(currentPlanId)
                    updateNodeStatuses(statusJson)
                } catch (_: Exception) { /* engine not ready */ }
                delay(300L)
            }
        }
    }

    // ─── JSON Parsing ─────────────────────────────────────────────────────

    private fun parseDag(json: String): Pair<List<DagView.DagNode>, List<DagView.DagEdge>> {
        val obj = JSONObject(json)
        val nodesArray = obj.optJSONArray("nodes") ?: JSONArray()
        val edgesArray = obj.optJSONArray("edges") ?: JSONArray()

        val nodes = (0 until nodesArray.length()).map { i ->
            val n = nodesArray.getJSONObject(i)
            DagView.DagNode(
                id = n.getString("id"),
                label = n.getString("label"),
                icon = n.optString("icon", ""),
                status = parseStatus(n.optString("status", "idle"))
            )
        }

        val edges = (0 until edgesArray.length()).map { i ->
            val e = edgesArray.getJSONObject(i)
            DagView.DagEdge(
                fromId = e.getString("from"),
                toId = e.getString("to")
            )
        }

        return nodes to edges
    }

    private fun parseStatus(status: String): DagView.NodeStatus = when (status) {
        "active" -> DagView.NodeStatus.ACTIVE
        "done" -> DagView.NodeStatus.DONE
        "alert", "error" -> DagView.NodeStatus.ALERT
        else -> DagView.NodeStatus.IDLE
    }

    private fun parseActivePlanId(agentsJson: String): Long {
        return try {
            val obj = JSONObject(agentsJson)
            obj.optLong("activePlanId", -1L)
        } catch (_: Exception) {
            -1L
        }
    }

    private fun updateNodeStatuses(statusJson: String) {
        try {
            val obj = JSONObject(statusJson)
            val nodesStatus = obj.optJSONArray("nodes") ?: return
            for (i in 0 until nodesStatus.length()) {
                val n = nodesStatus.getJSONObject(i)
                val id = n.getString("id")
                val status = parseStatus(n.getString("status"))
                binding.dagView.updateNodeStatus(id, status)
            }
        } catch (_: Exception) { /* malformed status */ }
    }

    // ─── Constants ──────────────────────────────────────────────────────

    companion object {
        const val EXTRA_PLAN_ID = "extra_plan_id"
    }
}
