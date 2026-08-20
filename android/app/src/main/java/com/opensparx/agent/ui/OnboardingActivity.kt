package com.opensparx.agent.ui

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView
import androidx.viewpager2.widget.ViewPager2
import com.opensparx.agent.R

/**
 * First-launch onboarding flow — 3 swipeable pages introducing
 * the on-device Agent OS to investors, developers, and partners.
 */
class OnboardingActivity : AppCompatActivity() {

    private lateinit var viewPager: ViewPager2
    private lateinit var dotsContainer: LinearLayout
    private lateinit var btnNext: TextView

    private val pageLayouts = intArrayOf(
        R.layout.onboarding_page1,
        R.layout.onboarding_page2,
        R.layout.onboarding_page3
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_onboarding)

        // Go full-screen immersive
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        )
        window.statusBarColor = 0x000D0D0D.toInt()
        window.navigationBarColor = 0xFF0D0D0D.toInt()

        viewPager = findViewById(R.id.viewPager)
        dotsContainer = findViewById(R.id.dotsContainer)
        btnNext = findViewById(R.id.btnNext)

        viewPager.adapter = OnboardingPagerAdapter()
        viewPager.registerOnPageChangeCallback(object : ViewPager2.OnPageChangeCallback() {
            override fun onPageSelected(position: Int) {
                updateDots(position)
                btnNext.text = if (position == pageLayouts.size - 1) "Enter" else "Next"
            }
        })

        setupDots()
        updateDots(0)

        btnNext.setOnClickListener {
            val current = viewPager.currentItem
            if (current < pageLayouts.size - 1) {
                viewPager.currentItem = current + 1
            } else {
                completeOnboarding()
            }
        }
    }

    private fun setupDots() {
        dotsContainer.removeAllViews()
        for (i in pageLayouts.indices) {
            val dot = View(this).apply {
                val size = resources.displayMetrics.density.toInt() * 8
                layoutParams = LinearLayout.LayoutParams(size, size).apply {
                    marginEnd = (resources.displayMetrics.density * 8).toInt()
                }
                background = ContextCompat.getDrawable(
                    this@OnboardingActivity, R.drawable.bg_pulse_dot
                )
                alpha = 0.3f
            }
            dotsContainer.addView(dot)
        }
    }

    private fun updateDots(position: Int) {
        for (i in 0 until dotsContainer.childCount) {
            dotsContainer.getChildAt(i).alpha = if (i == position) 1.0f else 0.3f
        }
    }

    private fun completeOnboarding() {
        getSharedPreferences("sparx_prefs", MODE_PRIVATE)
            .edit()
            .putBoolean("onboarding_complete", true)
            .apply()

        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }

    // ─── Adapter ────────────────────────────────────────────────────────

    private inner class OnboardingPagerAdapter :
        RecyclerView.Adapter<OnboardingPagerAdapter.PageViewHolder>() {

        inner class PageViewHolder(view: View) : RecyclerView.ViewHolder(view)

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): PageViewHolder {
            val view = LayoutInflater.from(parent.context)
                .inflate(pageLayouts[viewType], parent, false)
            return PageViewHolder(view)
        }

        override fun onBindViewHolder(holder: PageViewHolder, position: Int) {
            // Static layouts — no binding needed
        }

        override fun getItemCount(): Int = pageLayouts.size

        override fun getItemViewType(position: Int): Int = position
    }
}
