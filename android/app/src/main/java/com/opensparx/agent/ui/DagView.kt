package com.opensparx.agent.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import kotlin.math.min

/**
 * DagView — Canvas-based directed acyclic graph visualization.
 *
 * Draws agent nodes as rounded rectangles with status indicators,
 * and edges as animated paths with flowing particles to represent
 * data movement between agents during plan execution.
 *
 * Supports progressive reveal: nodes/edges appear incrementally
 * as the plan unfolds via [revealUpTo].
 */
class DagView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    // ─── Data Model ─────────────────────────────────────────────────────

    data class DagNode(
        val id: String,
        val label: String,
        val icon: String = "",         // emoji or icon key
        var status: NodeStatus = NodeStatus.IDLE,
        var x: Float = 0f,
        var y: Float = 0f,
        var revealed: Boolean = false,
        var revealProgress: Float = 0f // 0..1 for fade-in
    )

    data class DagEdge(
        val fromId: String,
        val toId: String,
        var revealed: Boolean = false
    )

    enum class NodeStatus {
        IDLE, ACTIVE, DONE, ALERT
    }

    // ─── State ──────────────────────────────────────────────────────────

    private val nodes = mutableListOf<DagNode>()
    private val edges = mutableListOf<DagEdge>()
    private var revealIndex = 0

    // ─── Paints ─────────────────────────────────────────────────────────

    private val nodePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }

    private val nodeBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }

    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#E6EDF3")
        textSize = 28f
        typeface = Typeface.MONOSPACE
        textAlign = Paint.Align.CENTER
    }

    private val iconPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 32f
        textAlign = Paint.Align.CENTER
    }

    private val edgePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f
        color = Color.parseColor("#30363D")
    }

    private val particlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.parseColor("#58A6FF")
    }

    // ─── Animation ──────────────────────────────────────────────────────

    private var particlePhase = 0f

    private val edgeAnimator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1500L
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener { anim ->
            particlePhase = anim.animatedValue as Float
            invalidate()
        }
    }

    // ─── Dimensions ─────────────────────────────────────────────────────

    private val nodeWidth = 160f
    private val nodeHeight = 72f
    private val nodeCornerRadius = 14f
    private val particleRadius = 4.5f
    private val particleCount = 4

    // ─── Colors ─────────────────────────────────────────────────────────

    private val colorIdle = Color.parseColor("#484F58")
    private val colorActive = Color.parseColor("#D29922")
    private val colorDone = Color.parseColor("#2EA043")
    private val colorAlert = Color.parseColor("#F85149")
    private val nodeFillColor = Color.parseColor("#161B22")

    // ─── Public API ───────────────────────────────────────────────────────

    /** Replace the entire graph. Call from coroutine when plan JSON is parsed. */
    fun setGraph(newNodes: List<DagNode>, newEdges: List<DagEdge>) {
        nodes.clear()
        edges.clear()
        nodes.addAll(newNodes)
        edges.addAll(newEdges)
        revealIndex = 0
        layoutNodes()
        invalidate()
    }

    /** Progressively reveal nodes up to [index] with animation. */
    fun revealUpTo(index: Int) {
        val target = index.coerceIn(0, nodes.size)
        for (i in revealIndex until target) {
            nodes[i].revealed = true
            animateNodeReveal(nodes[i])
            // Reveal edges whose target is now visible
            edges.filter { it.toId == nodes[i].id || it.fromId == nodes[i].id }
                .forEach { edge ->
                    val from = nodes.find { it.id == edge.fromId }
                    val to = nodes.find { it.id == edge.toId }
                    if (from?.revealed == true && to?.revealed == true) {
                        edge.revealed = true
                    }
                }
        }
        revealIndex = target
        invalidate()
    }

    /** Update a node's status (drives border color). */
    fun updateNodeStatus(nodeId: String, status: NodeStatus) {
        nodes.find { it.id == nodeId }?.let {
            it.status = status
            invalidate()
        }
    }

    // ─── Layout ─────────────────────────────────────────────────────────

    /**
     * Simple top-down DAG layout. Arranges nodes in rows based on their
     * topological order, evenly spaced.
     */
    private fun layoutNodes() {
        if (nodes.isEmpty() || width == 0) return

        val cols = 2
        val hSpacing = width.toFloat() / (cols + 1)
        val vSpacing = (height.toFloat() - 120f) / ((nodes.size / cols) + 2)

        nodes.forEachIndexed { i, node ->
            val row = i / cols
            val col = i % cols
            node.x = hSpacing * (col + 1) - nodeWidth / 2
            node.y = 80f + vSpacing * (row + 1) - nodeHeight / 2
        }
    }

    // ─── Drawing ──────────────────────────────────────────────────────────

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutNodes()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        drawEdges(canvas)
        drawNodes(canvas)
    }

    private fun drawEdges(canvas: Canvas) {
        for (edge in edges) {
            if (!edge.revealed) continue

            val from = nodes.find { it.id == edge.fromId } ?: continue
            val to = nodes.find { it.id == edge.toId } ?: continue

            val startX = from.x + nodeWidth / 2
            val startY = from.y + nodeHeight
            val endX = to.x + nodeWidth / 2
            val endY = to.y

            // Draw the edge line
            canvas.drawLine(startX, startY, endX, endY, edgePaint)

            // Draw flowing particles along the edge
            drawParticles(canvas, startX, startY, endX, endY)
        }
    }

    private fun drawParticles(canvas: Canvas, x1: Float, y1: Float, x2: Float, y2: Float) {
        for (i in 0 until particleCount) {
            val t = ((particlePhase + i.toFloat() / particleCount) % 1f)
            val px = x1 + (x2 - x1) * t
            val py = y1 + (y2 - y1) * t
            // Fade particles at edges
            val alpha = (min(t, 1f - t) * 4f).coerceIn(0f, 1f)
            particlePaint.alpha = (alpha * 200).toInt()
            canvas.drawCircle(px, py, particleRadius, particlePaint)
        }
    }

    private fun drawNodes(canvas: Canvas) {
        for (node in nodes) {
            if (!node.revealed) continue

            val rect = RectF(node.x, node.y, node.x + nodeWidth, node.y + nodeHeight)
            val alpha = (node.revealProgress * 255).toInt()

            // Fill
            nodePaint.color = nodeFillColor
            nodePaint.alpha = alpha
            canvas.drawRoundRect(rect, nodeCornerRadius, nodeCornerRadius, nodePaint)

            // Border with status color
            nodeBorderPaint.color = statusColor(node.status)
            nodeBorderPaint.alpha = alpha
            canvas.drawRoundRect(rect, nodeCornerRadius, nodeCornerRadius, nodeBorderPaint)

            // Icon (top area)
            if (node.icon.isNotEmpty()) {
                iconPaint.alpha = alpha
                canvas.drawText(
                    node.icon,
                    node.x + nodeWidth / 2,
                    node.y + 30f,
                    iconPaint
                )
            }

            // Label (bottom area)
            labelPaint.alpha = alpha
            labelPaint.textSize = 22f
            canvas.drawText(
                node.label,
                node.x + nodeWidth / 2,
                node.y + nodeHeight - 14f,
                labelPaint
            )
        }
    }

    // ─── Helpers ──────────────────────────────────────────────────────────

    private fun statusColor(status: NodeStatus): Int = when (status) {
        NodeStatus.IDLE -> colorIdle
        NodeStatus.ACTIVE -> colorActive
        NodeStatus.DONE -> colorDone
        NodeStatus.ALERT -> colorAlert
    }

    private fun animateNodeReveal(node: DagNode) {
        ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 400L
            addUpdateListener { anim ->
                node.revealProgress = anim.animatedValue as Float
                invalidate()
            }
            start()
        }
    }

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        edgeAnimator.start()
    }

    override fun onDetachedFromWindow() {
        edgeAnimator.cancel()
        super.onDetachedFromWindow()
    }
}
