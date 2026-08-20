package com.opensparx.agent.ui

import android.animation.ObjectAnimator
import android.animation.ValueAnimator
import android.os.Bundle
import android.view.View
import android.view.animation.AccelerateDecelerateInterpolator
import androidx.appcompat.app.AppCompatActivity
import com.opensparx.agent.R

/**
 * Technology Showcase — displays the three core research technologies
 * powering the OpenSparX Agent OS with real eval metrics.
 */
class TechShowcaseActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_tech_showcase)

        setupPulseAnimation()
    }

    /**
     * Animates the green pulse dot on the Speculative Execution card
     * to indicate the engine is actively running.
     */
    private fun setupPulseAnimation() {
        val pulseView = findViewById<View>(R.id.pulse_indicator) ?: return

        val animator = ObjectAnimator.ofFloat(pulseView, "alpha", 1f, 0.2f).apply {
            duration = 1200
            repeatCount = ValueAnimator.INFINITE
            repeatMode = ValueAnimator.REVERSE
            interpolator = AccelerateDecelerateInterpolator()
        }
        animator.start()
    }
}
