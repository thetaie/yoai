# The JNI entry points are found by name at runtime, so nothing that a native
# method belongs to may be renamed or stripped.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# The two callback interfaces are instantiated from Kotlin but INVOKED from C
# through GetMethodID, by literal name and signature. R8 has no way to see that
# use, so it would happily rename onEvent/onAudio.
-keep class com.yoai.bodyclock.Yoai { *; }
-keep interface com.yoai.bodyclock.Yoai$ChatSink { *; }
-keep interface com.yoai.bodyclock.Yoai$AudioSink { *; }
