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

.PHONY: test android install-android ios podspec lint detekt ktlint kover vuln baseline-guard check help

help:
	@echo "test             shared unit tests (fast gate)"
	@echo "android          debug APK"
	@echo "install-android  test + APK + adb install (needs phone on USB)"
	@echo "podspec          shared podspec + dummy framework (before pod install)"
	@echo "ios              simulator build (needs CocoaPods + simulator runtime)"
	@echo "lint             Android Lint (gating, baselined)"
	@echo "detekt           static analysis (gating, baselined)"
	@echo "ktlint           format check (advisory, violations logged)"
	@echo "kover            coverage report (no bounds, see build/reports/kover)"
	@echo "check            shared tests + assemble + APK + lint (gating, baselined)"
	@echo "vuln             dependency vulnerability scan (advisory, SARIF in build/osv-scanner)"
	@echo "baseline-guard no-growth gate on lint/detekt baselines (caps in tools/baseline-guard.sh)"

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

# Static analysis (detekt gating, ktlint advisory — see ARCHITECTURE.md
# section 5): lint + detekt fail the build on NEW issues (baselines absorb
# triaged ones); ktlint/kover stay green via ignoreFailures (ktlint) and no
# kover bounds. NOT part of `check` except via :androidApp:lintDebug;
# promotion of ktlint is tracked in ARCHITECTURE.md.
detekt:
	./gradlew detekt --console=plain

ktlint:
	./gradlew ktlintCheck --console=plain

# Kover: report tasks only, no verification bounds. Scoped to :shared (the only
# module with unit tests); root aggregation stays empty for KMP variants —
# revisit aggregation before setting any bound.
kover:
	./gradlew :shared:koverXmlReport --console=plain

# Vulnerability scanning (advisory only). Target is named `vuln` (not
# `osv-scan`): short name matching the detekt/ktlint/kover group. Standalone by design — NOT part of `check`, and
# the scan step is `-`-tolerated so findings (or an offline CI box) never
# fail the build; the summary line keeps counts visible instead.
# Flow: ephemeral Gradle lockfiles (scan-only -PosvLocks, see root
# build.gradle.kts) -> osvScan (SARIF, auto-installs the binary, warn-only
# via count/threshold) -> print finding counts + keep a .sarif copy for CI
# upload -> delete lockfiles so none are ever committed.
vuln:
	./gradlew dependencies :shared:dependencies :androidApp:dependencies --write-locks -PosvLocks --console=plain
	-./gradlew osvScan --console=plain
	python3 -c "import json,pathlib,shutil; p=pathlib.Path('build/osv-scanner/osv-scanner-scan.txt'); r=json.loads(p.read_text()); res=r['runs'][0].get('results',[]); print(f\"osv-scan: {len(res)} findings, {len(set(x['ruleId'] for x in res))} distinct rules (advisory — see ARCHITECTURE.md section 5)\"); shutil.copy(p, p.with_suffix('.sarif'))" || echo "osv-scan: no report (scan step failed - offline?); wiring intact, rerun with network"
	rm -f gradle.lockfile settings-gradle.lockfile shared/gradle.lockfile androidApp/gradle.lockfile

# Baseline no-growth guard (see tools/baseline-guard.sh): fails when either
# baseline absorbs new findings. Caps live next to the check so bumping one
# is a deliberate reviewed diff. Local parity for the CI gate below.
baseline-guard:
	./tools/baseline-guard.sh

check:
	./gradlew :shared:jvmTest :shared:assemble :androidApp:assembleDebug :androidApp:lintDebug --console=plain
