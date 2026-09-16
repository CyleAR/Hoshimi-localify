# HoshimiLocalify iOS

2026-09-15: 사용자가 SideStore + iLoader로 복호화된 IDOLY PRIDE 6.0.2의 설치와 실행에 성공했다. 기존 Android의 IL2CPP 번역 훅·JSON 데이터와 폰트 자산 교체를 이식한다. AstralParty/프로토버프 방식은 사용하지 않는다.

## 현재 결과물: Hook v29

사용자 기기에서 v18의 화면·폰트·ADV 번역이 확인됐다. v23은 ADV 원문 파일에는 괄호 치환만 적용하고, Android와 같은 generic 텍스트 보정 단계에서 조사·대시·문장부호를 처리한다. 검증된 `Google.Protobuf.MessageExtensions.MergeFrom` 뒤에서 MasterDB의 직접 필드와 중첩 객체·배열을 모두 적용한다. 정적 폰트 경로는 교체한 `SourceSansPro-Regular`를 런타임에서 찾아 Android 방식으로 활성화하고, 모든 TMP 폰트의 폴백 목록에 등록한다. v29는 generic 분할 검색이 원문을 출력한 뒤 호출자가 다시 원문을 붙이던 중복 버그를 수정했다. 정적 게이트웨이 자체가 원인이라는 이전 판단은 잘못이었다. 제공된 Dobby는 디버거 스크립트가 처리하는 BRK 명령을 포함하므로 일반 IPA 실행 경로에서 제거했다. 여섯 텍스트 훅은 정적 게이트웨이로 연결하며, 부분 문자열도 Android처럼 지정 범위만 번역한다. 2026-09-16 사용자가 v29 설치 후 정상 실행과 문제없음을 확인했다. 이후 기능 추가의 성공 기준선으로 보존한다.

- Android와 같은 `localization.json`의 키 → 번역 문자열 4,185개를 내장한다. 빌드 때 UTF-16 검색 테이블로 변환하므로 별도의 JSON 파일 복사가 필요 없다.
- 실제 Android 훅 대상인 인스턴스 메서드 `Qua.UI.I18n.SetValue`에 연결한다. v1에서 찾은 정적 메서드 `I18nHelper.SetValue`는 이 메서드를 호출하는 래퍼다. 두 함수의 인자 배치를 구분하고 숨은 `MethodInfo` 인자도 보존한다.
- IPA 안의 `Data/sharedassets0.assets`에서 `SourceSansPro-Regular` Font 객체의 `m_FontData`를 한글 OTF로 교체한다. Android의 정적 폰트 교체와 같은 방식이다.
- 설정 → 앱 → 아이프라의 `한글패치 사용` 스위치로 다음 실행부터 번역과 폰트 활성화를 켜고 끈다. 기본값은 켜짐이다.
- IPA의 `HoshimiLocal/local-files/resource/adv`에서 같은 이름의 파일을 찾고, Android 구현과 같이 괄호를 전각 괄호로 바꿔 원래 완료 콜백에 전달한다. 파일이 없거나 콜백을 해석하지 못하면 원본 로더를 호출한다.
- MasterDB JSON은 패키징 때 Android의 flat rule 전체를 검증 가능한 `master.bin` 검색 인덱스로 변환한다. 현재 서브레포 기준 90개 테이블·290개 필드·220,902개 문자열이며 `levels[0].description`, `contents[0].text`, `stepInfo[0].texts` 같은 중첩 객체·배열도 포함한다.
- 루트와 `genericTrans` 아래의 `generic.json`·`generic.split.json`은 Android의 exact·format·split 맵과 `translatedText` 제외 목록을 그대로 컴파일한 `generic.bin`으로 내장한다. 현재 데이터는 완전일치 1,097개·형식문 242개·분할문 8개·이미 번역된 문자열 100,263개다. v29는 정적 게이트웨이로 `TMP_Text.set_text`, `SetText(String,bool)`, `PopulateTextBackingArray`, `SetCharArray`, Legacy UI.Text, UIElements TextField를 연결하고 조사·문장부호·`[center]` 보정도 텍스트 단위로 수행한다.

### 기기에서 확인할 것

1. 설치에 성공했던 **iLoader의 IPA 가져오기** 또는 SideStore로 v23 IPA를 재서명·설치한다. LiveContainer는 필요하지 않다.
2. 기본 Bundle ID는 Apple 규칙에 맞춘 `game.qualiarts.idolypride.kr`다. 같은 서명 계정과 Bundle ID로 설치하면 기존 아이프라 앱을 업데이트하는 용도다. 설치 도구가 ID를 변경하면 별도 앱으로 설치될 수 있다.
3. 이름이 **아이프라**인 앱을 실행하고 홈·메뉴 화면을 몇 군데 열어 본다.
4. 파일 앱 → 나의 iPad → **아이프라** → `hoshimi-ios-hook.log`를 확인한다. 로그 폴더가 안 보이면 Apple 기기 앱의 파일 공유에서도 확인한다.
5. 로그의 `ARMED Qua.UI.I18n.SetValue ... entries=4185`는 연결 준비 완료, `TRANSLATED hit=...`는 실제 번역 문자열을 원본 함수에 전달했다는 뜻이다.

로그는 실행마다 새로 쓰므로 **앱을 다시 켜기 전에 보관**한다. 처음 24개 번역 적중과 100·1,000번째 적중을 기록한다. 번역 키와 문자열 길이만 기록하며 게임이 표시하는 원문 값은 기록하지 않는다.

### Hook v23 빌드 및 검증

```powershell
chcp 65001 > $null
./ios/build-hook.ps1
python ios/tools/package_probe.py --ipa dump/ios/game.qualiarts.idolypride-6.0.2-Decrypted.ipa --dylib ios/build/hook-v2/HoshimiLocalify.dylib --hook-plan ios/build/hook-v2/hook-plan.json --font-file ios/PretendardJP-SemiBold.otf --local-data-root app/src/main/assets/hoshimi-local --include-adv --include-master --patch-revision 29 --output ios/build/IdolyPride-6.0.2-HoshimiHook-v29.ipa
```

출력 IPA가 이미 있으면 새 이름을 지정한다. 원본 IPA는 변경하지 않는다.

기준 폰트는 개발 폴더의 `ios/PretendardJP-SemiBold.otf`다. 출력 이름은 매번 새 이름을 사용한다.

`tools/static_hook.py`는 원본 UnityFramework 전체 SHA-256, IL2CPP 코드 등록 테이블, 래퍼의 분기와 대상 함수의 시작 명령을 검증한다. 이 입력에서 실제 대상 RVA는 `0x6efc218`이며, v1이 찾은 래퍼 RVA는 `0x6efd66c`다. 다른 게임 버전에는 그대로 적용할 수 없다.

### hoshimi-local 데이터 사용

iOS용 번역 원본도 Android와 같은 서브레포
`app/src/main/assets/hoshimi-local`을 사용한다. ADV와 MasterDB를 포함할 때 파일을
별도의 iOS 전용 번역 폴더로 복제하지 않는다. 패키저가 서브레포의 `version.txt`,
`local-files/resource/adv`, `local-files/masterTrans`, `local-files/genericTrans`를
검증하고, MasterDB와 generic은 Android 맵을 컴파일한 인덱스로 만들어 IPA의
`HoshimiLocal` 폴더에 넣는다.

```powershell
chcp 65001 > $null
python ios/tools/package_probe.py --ipa dump/ios/game.qualiarts.idolypride-6.0.2-Decrypted.ipa --dylib ios/build/hook-v2/HoshimiLocalify.dylib --hook-plan ios/build/hook-v2/hook-plan.json --font-file ios/PretendardJP-SemiBold.otf --local-data-root app/src/main/assets/hoshimi-local --include-adv --include-master --output ios/build/IdolyPride-6.0.2-HoshimiHook-with-data.ipa
```

현재 서브레포 기준 내장 대상은 ADV 3,005개, MasterDB 90개와 generic 인덱스다. 패키저가
원본 JSON을 그대로 복제하지 않고 빌드 시 Android와 동일한 인덱스로 변환한다. 별도 진단으로 얻은 주소를 정적 패치
대상으로 검증한 뒤 generic, ADV, MasterDB 순으로 활성화한다.

서브레포 자체의 GitHub Actions는 `version.txt`와 `local-files`를 Release ZIP으로
배포한다. 추후 자동 업데이트는 이 ZIP의 버전을 확인하고 앱 데이터 영역에 받은
업데이트본을 IPA 내장본보다 우선해서 읽는 구조로 맞춘다.

패키징 시 대상 함수의 첫 4바이트를 분기로 바꾸고, 모든 선언된 섹션 뒤의 검증된 실행 세그먼트 여유 공간에 게이트웨이를 넣는다. 데이터 포인터는 기존 객체 영역 밖에 있는 쓰기 가능한 세그먼트의 zero-fill 끝 여유 공간을 사용한다. 실행 중에는 이 포인터만 연결하며 코드 페이지 쓰기나 JIT를 사용하지 않는다. 포인터가 연결되기 전에는 원본 함수로 진행한다. `--font-file`을 지정하면 별도로 `sharedassets0.assets`의 `SourceSansPro-Regular` Font 데이터도 교체한다.

ARM64 에뮬레이션으로 I18n·폰트·ADV·MasterDB·generic 게이트웨이의 원본 함수 복귀, 연결 전후 인자 보존, 주소 재배치(ASLR)를 확인했다. 패키징과 두 인덱스 검증을 포함한 **34개 테스트가 통과**했다. 이는 iPad에서 전체 게임을 실행한 검증을 대신하지 않는다.

```powershell
chcp 65001 > $null
$env:PYTHONIOENCODING = 'utf-8'
python -m pip install --target ios/build/test-runtime --only-binary=:all: capstone unicorn
$env:PYTHONPATH = (Resolve-Path ios/build/test-runtime).Path
python -m unittest discover -s ios/tools -p 'test_*.py'
```

정적 폰트 교체 IPA를 설치한 뒤에도 네모가 보이면 `SourceSansPro-Regular`가 실제 표시 경로에서 사용되는지 로그와 화면을 함께 확인한다.

## 이전 결과물: Probe v1

`build/IdolyPride-6.0.2-HoshimiProbe-v1.ipa`는 **라이브러리 로딩과 IL2CPP 메서드 탐색을 확인하는 진단 IPA**다. 한글 번역이나 폰트 교체는 아직 포함하지 않는다.

1. iLoader 또는 SideStore에서 해당 IPA를 선택해 재서명·설치한다.
2. 이름이 **아이프라**인 앱을 실행해 타이틀 또는 홈 화면까지 진입한다. 앱을 전면에 둔 채 약 30초 기다린다.
3. 파일 앱 → 나의 iPad → **아이프라** → `hoshimi-ios-probe.log`를 꺼낸다. 탐색이 늦어지면 최대 6분 동안 기다린 뒤 타임아웃 이유를 기록한다.
4. 다음 실행 전 로그를 보관한다. 앱을 다시 켜면 로그는 새 실행 결과로 교체된다.

로그의 `PASS: resolver probe completed`와 `TARGET Qua.UI.I18nHelper.SetValue ... rva=...`가 다음 정적 훅 구현의 입력이다. 인자형도 함께 기록한다. `SetUpI18n` 후보도 같이 조사한다. 이 주소는 이 버전의 분석에만 사용하며, 다른 버전에 그대로 쓰지 않는다.

로그 폴더가 안 보이면 Apple 기기 앱의 파일 공유에서도 확인한다. 로그가 전혀 없거나 실행 직후 종료되면 라이브러리 로딩·서명부터 조사해야 한다. `PASS`는 번역 훅 성공을 뜻하지 않는다.

패키징 기본 Bundle ID는 `game.qualiarts.idolypride.kr`다. Apple Bundle ID에는 밑줄을 사용할 수 없다. 원본 게임과 같은 ID를 사용하면 설치 도구에서 기존 앱을 대체하거나 업데이트할 수 있으므로, 별도 설치가 필요할 때는 `game.qualiarts.idolypride.*` 형식의 테스트 ID를 직접 지정한다.

## Windows에서 다시 빌드

Android NDK 26.3.11579264의 Clang/LLD로 ARM64 Mach-O dylib를 만든다. NDK의 Android 라이브러리를 링크하지 않는다. 필요하면 `HOSHIMI_LLVM_BIN` 환경 변수에 LLVM 실행 파일 디렉터리를 지정한다.

```powershell
chcp 65001 > $null
./ios/build-probe.ps1
python ios/tools/package_probe.py --ipa dump/ios/game.qualiarts.idolypride-6.0.2-Decrypted.ipa --dylib ios/build/HoshimiProbe.dylib --output ios/build/IdolyPride-6.0.2-HoshimiProbe-v1.ipa
```

출력 파일이 이미 있으면 덮어쓰지 않고 중단한다. 재패키징할 때 새 출력 이름을 지정한다.

이 작은 C 진단 라이브러리는 공개 Darwin 함수의 최소 ABI 선언과 `libSystem.tbd` 링크용 심볼 목록만 사용하므로 macOS SDK 다운로드가 필요 없다. TBD에는 Apple 구현 코드가 없고, iPad의 `/usr/lib/libSystem.B.dylib`에서 함수를 찾도록 링크한다. 이 방법을 Objective-C++ 전체 패치나 iOS 폰트 번들 빌드에도 그대로 적용할 수 있다고 가정하지 않는다.

## 구현 및 검증 범위

- `probe/probe.c`: dyld 이미지 로딩 알림 → 별도 작업 스레드 → IL2CPP API 조회 → 기존 Android와 같은 어셈블리/클래스/메서드 탐색 → 로그. 이미지 로딩 콜백 안에서 Unity 호출을 수행하지 않는다.
- 메서드 엔트리는 Unity 2022의 `MethodInfo.methodPointer` 첫 필드를 읽은 후보이며, UnityFramework 내부 주소인지 검사한다. 후속 정적 패치에서 함수 바이트와 호출 규약을 다시 검증해야 한다.
- `tools/package_probe.py`: 검증된 원본 IPA SHA-256과 게임 버전, 암호화 해제 상태를 확인한다. UnityFramework의 0으로 채워진 헤더 여유 공간에 `LC_LOAD_DYLIB` 하나를 추가한다. 코드와 기존 주소는 이동하지 않는다.
- 진단 라이브러리는 `Frameworks/HoshimiProbe.dylib`에 들어가며, UnityFramework에서 `@loader_path/../HoshimiProbe.dylib`로 로드한다.
- 파일 공유를 켜고 기존 리소스 서명 파일을 제거한다. 기존 Mach-O 서명은 패키징 후 유효하지 않으므로 SideStore에서 **내장 라이브러리와 앱 모두 재서명**해야 한다.
- 패키징 후 ZIP CRC와 수정 바이너리·내장 라이브러리 바이트를 확인한다. 별도의 `.report.json`에 입출력 SHA-256을 기록한다.
- 헤더 변경 테스트: `python -m unittest discover -s ios/tools -p "test_*.py"`.

Probe v1은 사용자 기기에서 `PASS: resolver probe completed`가 기록되어 라이브러리 로딩과 메서드 탐색이 확인됐다.
