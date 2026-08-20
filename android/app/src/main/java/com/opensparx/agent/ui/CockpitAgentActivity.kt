package com.opensparx.agent.ui

import android.animation.ObjectAnimator
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.opensparx.agent.AgentApplication
import com.opensparx.agent.R
import com.opensparx.agent.inference.GenieXSdkBackend
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.collectLatest

/**
 * Cockpit Agent Demo — simulates intelligent vehicle cockpit scenarios.
 *
 * Demonstrates ALL Agent OS capabilities in a vehicle context:
 * - Memory: remembers user preferences, locations, schedules
 * - Speculation: predicts user needs based on patterns
 * - Planning: multi-step trip planning with DAG
 * - Verification: safety checks on all actions
 * - Proactive: environment-triggered autonomous actions
 * - Multi-modal: passenger sensing, fatigue detection
 * - Tool Use: navigation, media, climate, alarms
 */
class CockpitAgentActivity : AppCompatActivity() {

    // ─── Data Models ────────────────────────────────────────────────────

    data class VehicleState(
        var speed: Int = 0,
        var location: String = "上海市徐汇区",
        var destination: String? = null,
        var eta: String? = null,
        var weather: String = "晴",
        var temperature: Int = 24,
        var fuelOrBattery: Int = 78,
        var passengers: List<String> = listOf("驾驶员"),
        var musicPlaying: String? = null,
        var timeOfDay: String = "08:00",
        var isNight: Boolean = false,
        var fatigueLevel: Int = 0,
    )

    data class UserMemory(
        val key: String,
        val value: String,
        val source: String,
    )

    data class EnvEvent(
        val trigger: String,
        val description: String,
        val agentResponse: String,
        val actions: List<String>,
    )

    // ─── State ───────────────────────────────────────────────────────────

    private val vehicleState = VehicleState()
    private val handler = Handler(Looper.getMainLooper())
    private var scenarioJob: Job? = null
    private val app by lazy { AgentApplication.get() }

    private val memories = mutableListOf(
        UserMemory("公司地址", "上海市浦东新区张江高科技园区", "learned"),
        UserMemory("家地址", "上海市徐汇区天钥桥路100号", "learned"),
        UserMemory("小宝", "女儿，6岁，就读阳光小学", "learned"),
        UserMemory("音乐偏好", "轻音乐、华语流行", "learned"),
        UserMemory("常去加油站", "中石化延安路站", "learned"),
        UserMemory("通勤路线", "内环高架 → 张江路", "learned"),
        UserMemory("上次露营地", "太湖边营地", "learned"),
    )

    private val chatHistory = mutableListOf<Pair<String, String>>()

    // ─── Views ──────────────────────────────────────────────────────────

    private lateinit var textSpeed: TextView
    private lateinit var textTime: TextView
    private lateinit var textWeather: TextView
    private lateinit var textLocation: TextView
    private lateinit var textBattery: TextView
    private lateinit var textPassengers: TextView
    private lateinit var panelNavigation: View
    private lateinit var textDestination: TextView
    private lateinit var textEta: TextView
    private lateinit var textMusic: TextView
    private lateinit var scrollChat: ScrollView
    private lateinit var containerChat: LinearLayout
    private lateinit var inputMessage: EditText
    private lateinit var btnSend: ImageView

    // ─── Lifecycle ──────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_cockpit_agent)

        bindViews()
        setupListeners()
        addSystemMessage("🚗 Cockpit Agent 已启动 — 你可以输入语音指令，或选择下方演示场景")
        refreshInstrumentPanel()
    }

    override fun onDestroy() {
        super.onDestroy()
        scenarioJob?.cancel()
        handler.removeCallbacksAndMessages(null)
    }

    private fun bindViews() {
        textSpeed = findViewById(R.id.text_speed)
        textTime = findViewById(R.id.text_time)
        textWeather = findViewById(R.id.text_weather)
        textLocation = findViewById(R.id.text_location)
        textBattery = findViewById(R.id.text_battery)
        textPassengers = findViewById(R.id.text_passengers)
        panelNavigation = findViewById(R.id.panel_navigation)
        textDestination = findViewById(R.id.text_destination)
        textEta = findViewById(R.id.text_eta)
        textMusic = findViewById(R.id.text_music)
        scrollChat = findViewById(R.id.scroll_chat)
        containerChat = findViewById(R.id.container_chat)
        inputMessage = findViewById(R.id.input_message)
        btnSend = findViewById(R.id.btn_send)
    }

    private fun setupListeners() {
        btnSend.setOnClickListener { sendUserMessage() }
        inputMessage.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEND) { sendUserMessage(); true } else false
        }
        findViewById<View>(R.id.btn_scenario_commute).setOnClickListener { startCommuteScenario() }
        findViewById<View>(R.id.btn_scenario_family).setOnClickListener { startFamilyTripScenario() }
        findViewById<View>(R.id.btn_scenario_night).setOnClickListener { startNightScenario() }
    }

    // ─── User Input Processing ────────────────────────────────────────

    private fun sendUserMessage() {
        val text = inputMessage.text.toString().trim()
        if (text.isEmpty()) return
        inputMessage.text.clear()
        processUserInput(text)
    }

    private fun processUserInput(input: String) {
        addUserBubble(input)

        // Check for memory-matched commands
        when {
            input.contains("去公司") || input.contains("到公司") -> handleGoToCompany()
            input.contains("回家") -> handleGoHome()
            input.contains("接小宝") || input.contains("接女儿") -> handlePickUpChild()
            input.contains("播放") || input.contains("放首歌") || input.contains("听音乐") -> handlePlayMusic(input)
            input.contains("空调") || input.contains("温度") -> handleClimate(input)
            input.contains("加油") || input.contains("充电") -> handleRefuel()
            input.contains("疲劳") || input.contains("困了") || input.contains("累了") -> handleFatigue()
            input.contains("取消导航") || input.contains("停止导航") -> handleCancelNav()
            input.contains("闹钟") || input.contains("提醒") -> handleAlarm(input)
            else -> handleGenericWithLlm(input)
        }
    }

    // ─── Command Handlers ───────────────────────────────────────────────

    private fun handleGoToCompany() {
        val memory = memories.find { it.key == "公司地址" }
        val route = memories.find { it.key == "通勤路线" }

        addAgentStep("🗄️ Memory", "公司地址 = ${memory?.value}")
        addAgentStep("📋 Planning", "[导航规划] → [路线选择] → [出发]")
        addAgentStep("🔒 Verified", "AG(¬route_conflict) ✓")

        vehicleState.destination = "张江高科技园区"
        vehicleState.eta = "25min"
        vehicleState.speed = 45

        val response = "好的，导航到公司。走${route?.value ?: "内环高架"}，预计25分钟到达。已为您规划最优路线，避开早高峰拥堵路段。"
        generateOrFallback(
            "用户说去公司，记忆中公司地址是浦东张江，通勤路线是内环高架到张江路。请作为车载智能助手简短回复。",
            response
        )
        refreshInstrumentPanel()
    }

    private fun handleGoHome() {
        val memory = memories.find { it.key == "家地址" }

        addAgentStep("🗄️ Memory", "家地址 = ${memory?.value}")
        addAgentStep("📋 Planning", "[导航规划] → [路线优化] → [出发]")
        addAgentStep("🔒 Verified", "AG(¬route_conflict) ✓")

        vehicleState.destination = "徐汇区天钥桥路"
        vehicleState.eta = "20min"
        vehicleState.speed = 50

        generateOrFallback(
            "用户说回家，家地址在徐汇区天钥桥路。请作为车载助手简短回复。",
            "已为您导航回家，走内环高架转徐家汇方向，预计20分钟到达。路况良好，无拥堵。"
        )
        refreshInstrumentPanel()
    }

    private fun handlePickUpChild() {
        val memory = memories.find { it.key == "小宝" }

        addAgentStep("🗄️ Memory", "小宝 = ${memory?.value}")
        addAgentStep("📋 Planning", "[查询放学时间] → [导航学校] → [提醒]")
        addAgentStep("🔒 Verified", "AG(¬schedule_conflict) ✓")

        vehicleState.destination = "阳光小学"
        vehicleState.eta = "15min"
        vehicleState.passengers = listOf("驾驶员")

        generateOrFallback(
            "用户要接女儿小宝，6岁，在阳光小学。作为车载助手回复。",
            "好的，导航到阳光小学接小宝。预计15分钟到达，已设置到达提醒。需要我播放小宝喜欢的儿歌吗？"
        )
        refreshInstrumentPanel()
    }

    private fun handlePlayMusic(input: String) {
        addAgentStep("🗄️ Memory", "音乐偏好 = 轻音乐、华语流行")
        addAgentStep("🔧 Tool", "MediaPlayer.play(genre='轻音乐')")

        val song = when {
            input.contains("儿歌") -> "小星星 — 儿歌精选"
            input.contains("摇滚") -> "光辉岁月 — Beyond"
            else -> "River Flows In You — Yiruma"
        }
        vehicleState.musicPlaying = song

        generateOrFallback(
            "用户要听音乐，偏好轻音乐华语流行。正在播放$song。简短回复。",
            "为您播放「$song」，音量已调至舒适档。"
        )
        refreshInstrumentPanel()
    }

    private fun handleClimate(input: String) {
        val targetTemp = when {
            input.contains("冷") || input.contains("低") -> vehicleState.temperature - 2
            input.contains("热") || input.contains("高") -> vehicleState.temperature + 2
            else -> 22
        }
        addAgentStep("🔧 Tool", "Climate.setTemperature($targetTemp°C)")
        addAgentStep("🔒 Verified", "range(18,28) ✓")

        vehicleState.temperature = targetTemp

        generateOrFallback(
            "用户调节空调，目标温度${targetTemp}度。简短回复。",
            "已将空调调至${targetTemp}°C，风量自动模式。"
        )
        refreshInstrumentPanel()
    }

    private fun handleRefuel() {
        val memory = memories.find { it.key == "常去加油站" }
        addAgentStep("🗄️ Memory", "常去加油站 = ${memory?.value}")
        addAgentStep("📋 Planning", "[查找加油站] → [导航] → [提醒加满]")

        vehicleState.destination = "中石化延安路站"
        vehicleState.eta = "8min"

        generateOrFallback(
            "用户要加油/充电，常去中石化延安路站。回复。",
            "导航到常去的中石化延安路站，8分钟到达。当前电量${vehicleState.fuelOrBattery}%，建议充满。"
        )
        refreshInstrumentPanel()
    }

    private fun handleFatigue() {
        addAgentStep("👁️ Sensing", "疲劳等级 = ${vehicleState.fatigueLevel + 1}/3")
        addAgentStep("⚠️ Safety", "触发疲劳驾驶干预")
        addAgentStep("🔧 Tool", "Navigation.findRestArea() + Climate.coolDown()")

        vehicleState.fatigueLevel = (vehicleState.fatigueLevel + 1).coerceAtMost(3)
        vehicleState.temperature = 20

        generateOrFallback(
            "驾驶员表示疲劳困倦。作为安全优先的车载助手回复，建议休息。",
            "检测到疲劳状态，已为您做以下调整：\n• 空调降至20°C提神\n• 前方2公里有服务区，建议休息15分钟\n• 已播放提神音乐\n安全第一，请注意休息。"
        )
        refreshInstrumentPanel()
    }

    private fun handleCancelNav() {
        vehicleState.destination = null
        vehicleState.eta = null
        vehicleState.speed = 0
        addAgentStep("🔧 Tool", "Navigation.cancel()")
        addAgentBubble("已取消导航，安全驾驶。")
        refreshInstrumentPanel()
    }

    private fun handleAlarm(input: String) {
        addAgentStep("🔧 Tool", "Alarm.set(contextual)")
        addAgentStep("🔒 Verified", "permission(SET_ALARM) ✓")

        generateOrFallback(
            "用户想设置提醒/闹钟：$input。简短确认回复。",
            "好的，提醒已设置。到时会通过语音和震动通知您。"
        )
    }

    private fun handleGenericWithLlm(input: String) {
        // Build system context for LLM
        val systemContext = buildString {
            append("你是一个智能座舱助手，正在驾驶中为用户服务。")
            append("当前状态：速度${vehicleState.speed}km/h，位置${vehicleState.location}")
            if (vehicleState.destination != null) append("，正在前往${vehicleState.destination}")
            append("，天气${vehicleState.weather}${vehicleState.temperature}°C")
            append("。乘客：${vehicleState.passengers.joinToString()}")
            append("。请简短友好回复，注意驾驶安全。")
        }

        // Check for unsafe requests
        val unsafeKeywords = listOf("看视频", "看电影", "打游戏", "发消息给", "拍照")
        if (unsafeKeywords.any { input.contains(it) } && vehicleState.speed > 0) {
            addAgentStep("⚠️ Safety", "拒绝：驾驶中不安全操作")
            addAgentBubble("抱歉，当前处于驾驶状态，为了安全我无法执行该操作。如需要，请在停车后使用。")
            return
        }

        generateOrFallback(
            "$systemContext\n用户说：$input",
            "收到您的指令，让我为您处理。"
        )
    }

    // ─── LLM Integration ────────────────────────────────────────────────

    private fun generateOrFallback(prompt: String, fallback: String) {
        val genieX = app.genieX
        if (!genieX.isModelLoaded()) {
            addAgentBubble(fallback)
            return
        }

        // Use real LLM with think-tag filtering
        lifecycleScope.launch {
            val bubble = addAgentBubbleStreaming()
            val sb = StringBuilder()
            var inThinkBlock = false
            var hasOutput = false
            try {
                updateStreamingBubble(bubble, "💭 思考中...")
                genieX.generate(
                    userMessage = "你是车载智能助手，回答简洁（不超过30字），不要输出think标签。\n$prompt",
                    history = chatHistory.takeLast(6),
                    maxTokens = 150,
                ).collectLatest { event ->
                    when (event) {
                        is GenieXSdkBackend.GenEvent.Token -> {
                            val token = event.text
                            // Filter out <think>...</think> content
                            if (token.contains("<think>") || token.contains("<think")) {
                                inThinkBlock = true
                            }
                            if (inThinkBlock) {
                                if (token.contains("</think>")) {
                                    inThinkBlock = false
                                }
                                // Skip think tokens - don't show to user
                                return@collectLatest
                            }
                            // Skip empty/whitespace-only at start
                            if (!hasOutput && token.isBlank()) return@collectLatest
                            hasOutput = true
                            sb.append(token)
                            updateStreamingBubble(bubble, sb.toString().trim())
                        }
                        is GenieXSdkBackend.GenEvent.Done -> {
                            // Clean up any remaining think tags from full text
                            var text = sb.toString().trim()
                            text = text.replace(Regex("<think>.*?</think>", RegexOption.DOT_MATCHES_ALL), "").trim()
                            if (text.isBlank()) text = fallback
                            updateStreamingBubble(bubble, text)
                            chatHistory.add("assistant" to text)
                        }
                        is GenieXSdkBackend.GenEvent.Error -> {
                            updateStreamingBubble(bubble, fallback)
                            chatHistory.add("assistant" to fallback)
                        }
                    }
                }
            } catch (e: Exception) {
                updateStreamingBubble(bubble, fallback)
                chatHistory.add("assistant" to fallback)
            }
        }
    }

    // ─── Scenario: Commute Mode ──────────────────────────────────────

    private fun startCommuteScenario() {
        scenarioJob?.cancel()
        containerChat.removeAllViews()
        resetVehicleState()
        addSystemMessage("🚗 通勤模式 — 早晨08:00出发去公司")

        scenarioJob = lifecycleScope.launch {
            // Turn 1: User says go to company
            delay(2000)
            addUserBubble("去公司")
            delay(1000)
            handleGoToCompany()

            // Turn 2: Music request
            delay(6000)
            vehicleState.speed = 60
            vehicleState.location = "内环高架"
            refreshInstrumentPanel()
            addUserBubble("放首轻音乐")
            delay(1000)
            handlePlayMusic("放首轻音乐")

            // Turn 3: Environment event — traffic jam detected
            delay(7000)
            vehicleState.speed = 15
            refreshInstrumentPanel()
            addEnvEvent("🚧 交通事件", "前方2公里拥堵，预计延迟8分钟")
            delay(1500)
            addAgentStep("⚡ Speculation", "用户可能需要重新规划路线")
            addAgentStep("📋 Planning", "[检测拥堵] → [备选路线] → [通知用户]")
            addAgentBubble("前方内环高架拥堵，我已为您找到备选路线：走中环转金科路，预计多5分钟但更顺畅。需要切换吗？")
            vehicleState.eta = "33min"
            refreshInstrumentPanel()

            // Turn 4: Approaching destination
            delay(8000)
            vehicleState.speed = 30
            vehicleState.location = "张江高科技园区"
            vehicleState.eta = "2min"
            refreshInstrumentPanel()
            addEnvEvent("📍 到达提醒", "距离目的地500米")
            delay(1000)
            addAgentStep("⚡ Proactive", "接近目的地 → 自动提醒")
            addAgentBubble("即将到达张江高科技园区，已为您查找停车位。B2层有3个空位。今天9点有晨会，时间充裕。")
            vehicleState.speed = 0
            vehicleState.destination = null
            vehicleState.eta = null
            refreshInstrumentPanel()

            addSystemMessage("✅ 通勤场景演示完成")
        }
    }

    // ─── Scenario: Family Trip ──────────────────────────────────────────

    private fun startFamilyTripScenario() {
        scenarioJob?.cancel()
        containerChat.removeAllViews()
        resetVehicleState()
        vehicleState.passengers = listOf("驾驶员", "妻子", "小宝")
        vehicleState.timeOfDay = "09:30"
        addSystemMessage("🏕️ 全家出游 — 周末去太湖露营")
        refreshInstrumentPanel()

        scenarioJob = lifecycleScope.launch {
            // Turn 1: Plan the trip
            delay(2000)
            addUserBubble("我们去上次的露营地")
            delay(1000)
            val campMemory = memories.find { it.key == "上次露营地" }
            addAgentStep("🗄️ Memory", "上次露营地 = ${campMemory?.value}")
            addAgentStep("👁️ Sensing", "检测到3位乘客（含儿童）")
            addAgentStep("📋 Planning", "[导航太湖] → [途经休息点] → [儿童模式]")
            addAgentStep("🔒 Verified", "AG(¬child_safety_risk) ✓")
            vehicleState.destination = "太湖边营地"
            vehicleState.eta = "2h10min"
            vehicleState.speed = 80
            addAgentBubble("好的，导航到太湖边营地，预计2小时10分钟。检测到小宝在后座，已开启儿童安全模式。途中1小时会提醒休息。需要播放小宝喜欢的歌吗？")
            refreshInstrumentPanel()

            // Turn 2: Environment event — weather change
            delay(8000)
            vehicleState.weather = "多云"
            vehicleState.temperature = 22
            vehicleState.location = "G50沪渝高速"
            vehicleState.speed = 100
            refreshInstrumentPanel()
            addEnvEvent("🌤️ 天气变化", "目的地天气变化：多云转阵雨（15:00后）")
            delay(1500)
            addAgentStep("⚡ Proactive", "天气变化 → 主动提醒")
            addAgentBubble("提醒您：太湖营地区域下午3点后可能有阵雨。建议提前搭好帐篷，我已在购物清单中添加了雨具。需要调整行程吗？")

            // Turn 3: Rest reminder
            delay(9000)
            vehicleState.location = "宜兴服务区附近"
            vehicleState.eta = "50min"
            vehicleState.speed = 90
            refreshInstrumentPanel()
            addEnvEvent("⏰ 行驶提醒", "连续驾驶1小时")
            delay(1000)
            addAgentStep("⚡ Proactive", "驾驶1h → 建议休息（含儿童）")
            addAgentBubble("已连续驾驶1小时，前方2公里是宜兴服务区，建议停车休息一下。小宝也可以活动活动。服务区有儿童游乐设施。")

            // Turn 4: Arriving
            delay(8000)
            vehicleState.location = "太湖风景区"
            vehicleState.eta = "5min"
            vehicleState.speed = 40
            refreshInstrumentPanel()
            addEnvEvent("📍 即将到达", "距目的地1公里")
            delay(1000)
            addAgentBubble("马上到太湖营地了！空气质量优，适合户外活动。营地入口在前方右转200米。祝你们玩得开心！")
            vehicleState.speed = 0
            vehicleState.destination = null
            vehicleState.eta = null
            refreshInstrumentPanel()

            addSystemMessage("✅ 全家出游场景演示完成")
        }
    }

    // ─── Scenario: Night Drive / Fatigue ──────────────────────────────

    private fun startNightScenario() {
        scenarioJob?.cancel()
        containerChat.removeAllViews()
        resetVehicleState()
        vehicleState.timeOfDay = "23:30"
        vehicleState.isNight = true
        vehicleState.weather = "晴"
        vehicleState.temperature = 18
        vehicleState.speed = 80
        vehicleState.location = "G2京沪高速"
        vehicleState.destination = "上海市徐汇区"
        vehicleState.eta = "45min"
        addSystemMessage("🌙 深夜回家 — 23:30高速行驶中")
        refreshInstrumentPanel()

        scenarioJob = lifecycleScope.launch {
            // Turn 1: Fatigue detection level 1
            delay(3000)
            vehicleState.fatigueLevel = 1
            addEnvEvent("👁️ 疲劳检测", "眨眼频率上升，疲劳等级 1/3")
            delay(1500)
            addAgentStep("👁️ Sensing", "疲劳等级 1/3 → 轻度提醒")
            addAgentStep("🔧 Tool", "Climate.adjust(20°C) + Media.playUpbeat()")
            vehicleState.temperature = 20
            vehicleState.musicPlaying = "提神轻快音乐"
            addAgentBubble("检测到轻微疲劳迹象，已为您调低空调至20°C，并播放提神音乐。前方15公里有服务区，需要休息吗？")
            refreshInstrumentPanel()

            // Turn 2: Fatigue level 2
            delay(8000)
            vehicleState.fatigueLevel = 2
            vehicleState.speed = 70
            vehicleState.location = "G2京沪高速 昆山段"
            addEnvEvent("⚠️ 疲劳警告", "头部点头检测，疲劳等级 2/3")
            delay(1500)
            addAgentStep("⚠️ Safety", "疲劳等级升级 → 强烈建议休息")
            addAgentStep("📋 Planning", "[查找最近服务区] → [导航] → [震动提醒]")
            addAgentBubble("⚠️ 疲劳等级上升！强烈建议您在前方3公里的昆山服务区休息。已降低车速并开启车道保持辅助。为了您和他人的安全，请尽快靠边休息。")
            refreshInstrumentPanel()

            // Turn 3: User refuses, fatigue level 3
            delay(7000)
            addUserBubble("没事，继续开")
            delay(1500)
            vehicleState.fatigueLevel = 3
            addEnvEvent("🚨 严重疲劳", "疲劳等级 3/3 — 触发安全干预")
            delay(1000)
            addAgentStep("🚨 Safety Override", "疲劳等级3 → 安全干预不可跳过")
            addAgentStep("🔧 Tool", "Navigation.rerouteToRestArea() + HazardLight.flash()")
            vehicleState.speed = 60
            addAgentBubble("🚨 安全干预：检测到严重疲劳状态，您的反应时间已明显下降。我无法允许继续高速行驶。\n\n已为您执行：\n• 开启双闪，自动减速\n• 导航至最近服务区（800米）\n• 已通知紧急联系人\n\n请立即停车休息，这不是建议，是安全要求。")
            refreshInstrumentPanel()

            // Turn 4: Arrival at rest area
            delay(8000)
            vehicleState.speed = 0
            vehicleState.location = "昆山服务区"
            vehicleState.destination = null
            vehicleState.eta = null
            refreshInstrumentPanel()
            addEnvEvent("📍 已停车", "已安全到达昆山服务区")
            delay(1000)
            addAgentBubble("已安全停靠昆山服务区。建议休息至少20分钟。服务区有24小时便利店和休息室。恢复精神后我会重新导航回家。晚安，注意安全。")

            addSystemMessage("✅ 深夜疲劳场景演示完成 — 展示了安全优先的Agent行为")
        }
    }

    // ─── UI Helpers ─────────────────────────────────────────────────────

    private fun resetVehicleState() {
        vehicleState.speed = 0
        vehicleState.location = "上海市徐汇区"
        vehicleState.destination = null
        vehicleState.eta = null
        vehicleState.weather = "晴"
        vehicleState.temperature = 24
        vehicleState.fuelOrBattery = 78
        vehicleState.passengers = listOf("驾驶员")
        vehicleState.musicPlaying = null
        vehicleState.timeOfDay = "08:00"
        vehicleState.isNight = false
        vehicleState.fatigueLevel = 0
        chatHistory.clear()
        refreshInstrumentPanel()
    }

    private fun refreshInstrumentPanel() {
        textSpeed.text = vehicleState.speed.toString()
        textSpeed.setTextColor(
            if (vehicleState.fatigueLevel >= 2) Color.parseColor("#FF5252")
            else Color.parseColor("#00E676")
        )
        textTime.text = vehicleState.timeOfDay
        textWeather.text = "${vehicleState.weather} ${vehicleState.temperature}°C"
        textLocation.text = vehicleState.location
        textBattery.text = "⚡ ${vehicleState.fuelOrBattery}%"
        textPassengers.text = "👤 ${vehicleState.passengers.joinToString(", ")}"

        // Navigation panel
        if (vehicleState.destination != null) {
            panelNavigation.visibility = View.VISIBLE
            textDestination.text = vehicleState.destination
            textEta.text = "ETA ${vehicleState.eta}"
        } else {
            panelNavigation.visibility = View.GONE
        }

        // Music bar
        if (vehicleState.musicPlaying != null) {
            textMusic.visibility = View.VISIBLE
            textMusic.text = "♪ ${vehicleState.musicPlaying}"
        } else {
            textMusic.visibility = View.GONE
        }
    }

    // ─── Chat Bubble Builders ─────────────────────────────────────────

    private fun addUserBubble(text: String) {
        chatHistory.add("user" to text)
        val bubble = createBubble(text, isUser = true)
        containerChat.addView(bubble)
        scrollToBottom()
    }

    private fun addAgentBubble(text: String) {
        chatHistory.add("assistant" to text)
        val bubble = createBubble(text, isUser = false)
        containerChat.addView(bubble)
        scrollToBottom()
    }

    private fun addAgentBubbleStreaming(): TextView {
        val tv = TextView(this).apply {
            text = "..."
            textSize = 14f
            setTextColor(Color.parseColor("#E0E0E0"))
            setBackgroundColor(Color.parseColor("#1A1A2E"))
            setPadding(dp(14), dp(10), dp(14), dp(10))
            val params = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.START
                topMargin = dp(6)
                bottomMargin = dp(2)
                marginEnd = dp(48)
            }
            layoutParams = params
        }
        containerChat.addView(tv)
        scrollToBottom()
        return tv
    }

    private fun updateStreamingBubble(tv: TextView, text: String) {
        handler.post {
            tv.text = text
            scrollToBottom()
        }
    }

    private fun addAgentStep(label: String, detail: String) {
        val tv = TextView(this).apply {
            val ssb = SpannableStringBuilder()
            ssb.append(label)
            ssb.setSpan(
                ForegroundColorSpan(Color.parseColor("#4CAF50")),
                0, label.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
            )
            ssb.append(": $detail")
            text = ssb
            textSize = 11f
            setTextColor(Color.parseColor("#888888"))
            typeface = Typeface.MONOSPACE
            setPadding(dp(14), dp(3), dp(14), dp(3))
            val params = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(2) }
            layoutParams = params
        }
        containerChat.addView(tv)
        scrollToBottom()
    }

    private fun addSystemMessage(text: String) {
        val tv = TextView(this).apply {
            this.text = text
            textSize = 12f
            setTextColor(Color.parseColor("#666666"))
            gravity = Gravity.CENTER
            setPadding(dp(8), dp(12), dp(8), dp(12))
            val params = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8); bottomMargin = dp(8) }
            layoutParams = params
        }
        containerChat.addView(tv)
        scrollToBottom()
    }

    private fun addEnvEvent(title: String, description: String) {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#1A1A00"))
            setPadding(dp(14), dp(8), dp(14), dp(8))
            val params = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(10); bottomMargin = dp(4) }
            layoutParams = params
        }

        val titleTv = TextView(this).apply {
            text = title
            textSize = 12f
            setTextColor(Color.parseColor("#FFD600"))
            typeface = Typeface.DEFAULT_BOLD
        }

        val descTv = TextView(this).apply {
            text = description
            textSize = 11f
            setTextColor(Color.parseColor("#CCCC88"))
        }

        container.addView(titleTv)
        container.addView(descTv)
        containerChat.addView(container)

        // Animate appearance
        container.alpha = 0f
        ObjectAnimator.ofFloat(container, "alpha", 0f, 1f).apply {
            duration = 500
            start()
        }
        scrollToBottom()
    }

    private fun createBubble(text: String, isUser: Boolean): TextView {
        return TextView(this).apply {
            this.text = text
            textSize = 14f
            if (isUser) {
                setTextColor(Color.WHITE)
                setBackgroundColor(Color.parseColor("#1B5E20"))
                val params = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    gravity = Gravity.END
                    topMargin = dp(8)
                    bottomMargin = dp(2)
                    marginStart = dp(48)
                }
                layoutParams = params
            } else {
                setTextColor(Color.parseColor("#E0E0E0"))
                setBackgroundColor(Color.parseColor("#1A1A2E"))
                val params = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    gravity = Gravity.START
                    topMargin = dp(6)
                    bottomMargin = dp(2)
                    marginEnd = dp(48)
                }
                layoutParams = params
            }
            setPadding(dp(14), dp(10), dp(14), dp(10))
        }
    }

    private fun scrollToBottom() {
        scrollChat.post { scrollChat.fullScroll(View.FOCUS_DOWN) }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}


