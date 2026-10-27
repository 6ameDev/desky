# desky-client app builds (thin forwarder, house style: cf. embedded/Makefile).
# Toolchain env is exported once here (mise shims + ANDROID_HOME) so a stale
# shell never silently builds with the wrong JDK/SDK (T0b lesson); make
# propagates exports to every recipe shell, unlike inline VAR= assignments.
# iOS iteration tip: COCOAPODS_SKIP_KOTLIN_BUILD=YES in the Xcode scheme skips
# the Kotlin rebuild on pure-Swift edits (see ARCHITECTURE.md §5) — never in CI.

export PATH := $(HOME)/.local/share/mise/shims:/opt/homebrew/bin:$(PATH)
export ANDROID_HOME := $(HOME)/Library/Android/sdk
export ANDROID_SDK_ROOT := $(HOME)/Library/Android/sdk
export PATH := $(HOME)/Library/Android/sdk/cmdline-tools/latest/bin:$(HOME)/Library/Android/sdk/platform-tools:$(PATH)

APK = androidApp/build/outputs/apk/debug/androidApp-debug.apk

.PHONY: test android install-android ios podspec lint detekt ktlint kover check help

help:
	@echo "test             shared unit tests (fast gate)"
	@echo "android          debug APK"
	@echo "install-android  test + APK + adb install (needs phone on USB)"
	@echo "podspec          shared podspec + dummy framework (before pod install)"
	@echo "ios              simulator build (needs CocoaPods + simulator runtime)"
	@echo "lint             Android Lint (warn-only Phase 0, baselined)"
	@echo "detekt           static analysis (advisory Phase 1, baselined)"
	@echo "ktlint           format check (advisory Phase 1, violations logged)"
	@echo "kover            coverage report (no bounds Phase 1, see build/reports/kover)"

test:
	./gradlew :shared:jvmTest --console=plain

android:
	./gradlew :androidApp:assembleDebug --console=plain

install-android: test android
	adb install -r $(APK)

podspec:
	./gradlew :shared:podspec :shared:generateDummyFramework --console=plain

ios: podspec
	cd iosApp && pod install
	cd iosApp && xcodebuild -workspace iosApp.xcworkspace -scheme iosApp \
	  -configuration Debug \
	  -destination 'platform=iOS Simulator,name=iPhone 16,arch=arm64' \
	  -parallelizeTargets -jobs $$(sysctl -n hw.ncpu) \
	  COMPILER_INDEX_STORE_ENABLE=NO build

lint:
	./gradlew :androidApp:lintDebug --console=plain

# Phase 1 static analysis (issue 54, task 5): advisory only — tasks stay green
# via ignoreFailures (detekt/ktlint) and no kover bounds. NOT part of `check`;
# promotion to gating is task 9.
detekt:
	./gradlew detekt --console=plain

ktlint:
	./gradlew ktlintCheck --console=plain

# Kover: report tasks only, no verification bounds. Scoped to :shared (the only
# module with unit tests); root aggregation stays empty for KMP variants —
# revisit aggregation in task 9.
kover:
	./gradlew :shared:koverXmlReport --console=plain

check:
	./gradlew :shared:jvmTest :shared:assemble :androidApp:assembleDebug :androidApp:lintDebug --console=plain
