package de.robv.android.xposed;

/**
 * The subset of the Xposed API (version 82) the companion uses, with the same
 * names and descriptors as the real one. It serves twice: the module compiles
 * against it (it is never packaged into the module's classes.dex, so under
 * LSPosed the real classes are the ones that resolve), and the AVD dev loader
 * implements it on top of JVMTI breakpoints.
 */
public interface IXposedMod {}
