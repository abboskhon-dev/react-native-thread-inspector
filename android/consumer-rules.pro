# JNI looks these up by name.
-keep class com.threadinspector.NativeBridge {
    public static boolean postProbe(int, long);
    public static void onIncidentSaved(long, java.lang.String);
    native <methods>;
}
-keepclasseswithmembernames class com.threadinspector.** {
    native <methods>;
}
