# ProGuard / R8 rules for OpenSparX Agent

# Keep JNI bridge methods (called from native)
-keep class com.opensparx.agent.jni.AgentBridge { *; }

# Keep Application class
-keep class com.opensparx.agent.AgentApplication { *; }

# Keep all Service classes (launched by system)
-keep class * extends android.app.Service

# Keep all Activity classes
-keep class * extends android.app.Activity
