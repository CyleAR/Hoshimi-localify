# iOS 한글패치 Android 기능 대조 및 이식 계획

기준은 `app/src/main/cpp/HoshimiLocalify`와
`app/src/main/assets/hoshimi-local`이다. 게임 카메라·그래픽·FPS·의상 해금 같은 편의/개조
기능은 이 문서의 이식 범위에서 제외하고, 번역에 직접 필요한 경로만 비교한다.

## 현재 기준선

iOS v29 소스에는 다음 기능이 들어 있다.

| Android 기능 | iOS v29 소스 | 비고 |
|---|---|---|
| 전체 한글패치 사용 여부 | 구현 | iOS 설정 앱에서 제어 |
| `localization.json` | 구현 | `Qua.UI.I18n.SetValue`, 4,185개 |
| 정적 폰트 교체 방식 1 | 구현 | `SourceSansPro-Regular` 내부 폰트를 Pretendard로 교체하고 TMP 폴백에 등록 |
| ADV 교체 | 구현 | `resource/adv` 3,005개를 IPA에 내장 |
| MasterDB 전체 테이블 | 구현 | 90개 테이블, 290개 필드, 문자열 220,902개를 `master.bin`으로 컴파일 |
| generic 완전일치/형식문/분할문 | 구현 | exact 1,097개, format 242개, split 8개 |
| 이미 번역된 문장 재번역 방지 | 구현 | 100,263개 문자열 인덱스 |
| 조사 처리 | 구현 | Android의 조사 규칙을 텍스트 최종 단계에 적용 |
| 쉼표·Two-Em Dash·`[center]` 보정 | 구현 | Android의 `FixLigature`와 레이아웃 처리에 대응 |
| MasterDB 사용 스위치 | 구현 | 실제 Master 훅과 연결됨 |
| 이미지 교체 스위치 | v30 실기 확인 | 네 이미지 훅 및 PNG 836개와 연결 |
| 전화 자막 스위치 | 설정만 구현 | 아직 오디오/자막 훅과 연결되지 않음 |

`master.bin`과 `generic.bin`은 Android JSON을 다른 방식으로 새로 해석한 데이터가 아니다.
Android 서브레포의 같은 JSON을 IPA 빌드 시 읽기 전용 인덱스로 바꾼 것이다. 데이터 출처와
검색 순서는 Android와 같고, iOS에서 대용량 JSON 파싱을 피하기 위한 저장 형식만 다르다.

v29는 숫자 HUD의 반복 원인을 generic 분할 검색의 원문 중복 append로 확인하고 수정했다.
훅 수를 줄여 증상이 완화됐던 것은 같은 버그를 통과하는 횟수가 줄었기 때문이다.
제공된 Dobby는 디버거 스크립트가 처리하는 BRK 명령을 포함하므로 일반 IPA 경로에서 사용하지 않는다.
localization·폰트·ADV·MasterDB와 여섯 텍스트 경로를 정적 게이트웨이로 연결한다.
실제 ARM64 번역 코드에서 숫자/태그 문자열의 4회 반복 처리와 exact/format/split/조사/대시를 검사했다.
2026-09-16 사용자가 v29 실기 실행 후 문제없음을 확인했다. 이 버전을 성공 기준선으로 보존한다.

## 후속 기능과 검증 상태

### 1. 이미지 교체 — v30 실기 확인

Android는 `local-files/resource/img`의 PNG를 다음 네 경로에서 적용한다.

- `UnityEngine.UI.Image.set_sprite`
- `UnityEngine.UI.Image.set_overrideSprite`
- `UnityEngine.UI.RawImage.set_texture`
- `UnityEngine.UI.Graphic.OnEnable`

현재 번역 데이터에는 PNG 836개가 있다. iOS probe v4에서 네 진입 주소는 찾았다.

- `Image.set_sprite`: `0x797489c`
- `Image.set_overrideSprite`: `0x79843bc`
- `RawImage.set_texture`: `0x7b2c9a8`
- `Graphic.OnEnable`: `0x797eea0`

v30은 보조 API를 첫 관리형 UI 콜백에서 정확한 인자 타입으로 조회한다.
Texture2D/LoadImage/Sprite.Create는 runtime_invoke로 호출하여 구조체 인자를 전달하고,
원본 rect로 정규화한 pivot, border, pixels-per-unit을 보존한다. PNG 전체 크기를 새 rect로 쓴다.
Texture Clamp, DontUnloadUnusedAsset, preserveAspect는 Android 기본값과 같다.
생성 자산은 GC handle로 참조를 유지하며 Unity 객체 생존 확인 후 이름별 캐시를 재사용한다.
파일 누락·관리형 예외·API 누락 시 원본을 유지한다. 이미지 OFF는 다음 실행부터 적용된다.
PNG 836개를 `--include-images` 옵션으로 IPA에 포함한다. v29 성공 기준선은 dc5b6f6이다.
2026-09-16 사용자가 이미지 교체 정상 동작을 확인했다. 홈 로딩 시 약한 스터터링은 남아 있으며,
첫 이미지 읽기·디코딩·생성 비용을 계측한 뒤 최적화할 예정이다. 아직 원인이 확정된 것은 아니다.


### 2. 전화 자막

Android는 `phoneSubtitles.json`을 읽어 전화 음성 클립 재생을 감지하고, 재생 시간에 맞춰
TMP 자막을 갱신한다. 현재 데이터는 514개 클립, 4,681줄, 약 711 KB다.

필요한 재생 진입 주소는 probe v4에서 찾았다.

- `AudioSource.Play()`: `0x77d1554`
- `AudioSource.Play(UInt64)`: `0x77d15ac`
- `AudioSource.PlayDelayed(Single)`: `0x77d1610`
- `AudioSource.PlayOneShot(AudioClip,Single)`: `0x77d16f0`
- `AudioSource.set_clip`: `0x77d143c`
- `Object.get_name`: `0x7843a14`
- `Time.get_realtimeSinceStartup`: `0x78471e8`

아직 없는 것은 매 프레임 자막을 갱신할 안전한 main-thread 진입점과 TMP 오버레이 생성에
필요한 보조 API다. Android의 `EndCameraRendering`을 그대로 쓰되 iOS 주소를 추가로 찾는
것을 우선안으로 한다. `phoneSubtitles.json`은 같은 내용을 빌드 시 이진 인덱스로 바꿔
내장할 수 있다.

### 3. 사용자명 치환

Android의 `displayUserName` 설정과 다음 경로가 iOS에는 없다.

- ADV의 플레이어 이름 getter
- `MessageDetail.GetReplacedMessage`
- `MessageDetail.GetNotificationText`
- Master/ADV 문장 속 사용자명 자리표시자 치환 후 조사 처리

메시지 두 함수의 주소는 각각 `0x2760008`, `0x275fdc0`으로 확보했다. ADV 사용자명 getter는
probe v4에서 클래스 탐색이 실패했으므로 실제 구현 클래스와 메서드를 다시 찾아야 한다.
iOS 설정에는 사용자명 입력 칸도 추가해야 한다.

### 4. 일부 텍스트 진입 경로의 동등성

iOS는 현재 `TMP_Text.set_text`, `SetText(String,Boolean)`, 문자열
`PopulateTextBackingArray`, `SetCharArray`, UIElements `TextField`, Legacy `UI.Text`를
처리한다. Android는 여기에 `TextMeshProUGUI.Awake`에서 이미 들어 있던 초기 텍스트도 다시
검사한다. iOS 주소 `0x770d17c`는 확보되어 있으므로, 초기 직렬화 텍스트가 빠지는 화면이
확인되면 같은 훅을 추가한다.

Android 코드가 찾는 `TMP_Text.SetText(String)`은 iOS probe v4 메서드 목록에 별도 오버로드로
나타나지 않았다. iOS 버전에서는 `set_text` 또는 `SetText(String,Boolean)`으로 합쳐졌을 수
있으므로 존재하지 않는 훅을 억지로 추가하지 않고 호출 범위를 로그로 확인한다.

`I18nHelper.SetUpI18n`은 Android에서 훅하지만 현재 번역이 성공한 핵심 경로는 양쪽 모두
`I18n.SetValue`다. `I18n.GetOrDefault`는 Android에서도 설치 코드가 주석 처리되어 있다.
두 함수는 누락 기능으로 바로 이식하지 않고, 실제 일본어 잔존 화면이 이 경로를 쓸 때만
추가한다.

### 5. 새 번역 데이터 감지와 갱신

현재 iOS IPA는 빌드 시점의 `hoshimi-local` 데이터를 내장한다. Android처럼 외부 앱에서
파일을 바로 바꾸는 기능과 새 데이터 다운로드는 아직 없다. 이후에는 다음 우선순위로
구현한다.

1. 내장 데이터의 버전·해시 manifest 생성
2. 앱 컨테이너의 Application Support에 더 최신 데이터가 있으면 그것을 우선 사용
3. 다운로드 실패나 손상 시 내장 데이터로 자동 복귀
4. 필요하면 별도 관리 앱 또는 SideStore용 모듈 패키지에서 데이터만 갱신

네트워크 갱신은 이미지와 전화 자막까지 안정화한 뒤 진행한다. 먼저 데이터 검색 경로를
내장/외부 공통으로 만들어야 기능별 구현을 두 번 하지 않는다.

## 작업 순서

### 0단계: v26 Dobby 텍스트 훅 기기 검증

- 홈의 매니저 수치, 재화, VENUS RANK, 시간, 남은 일수가 한 번만 표시되는지 확인
- localization, ADV, 전체 MasterDB, generic, 조사, Two-Em Dash가 그대로 유지되는지 확인
- 로그에서 각 데이터 인덱스의 로드 성공과 훅 ARMED를 확인
- 카드·캐릭터·의상 외에 공지/메시지/상점/라이브 관련 Master 화면도 표본 확인
- ADV에서 `⸺`가 `—`로 표시되고 조사 표지가 남지 않는지 확인

완료 조건은 강제 종료 없이 기존 성공 화면과 위 항목이 모두 동작하는 것이다.

### 1단계: 텍스트 경로와 사용자명 동등성 완성

- `TextMeshProUGUI.Awake`를 별도 빌드로 추가하고 초기 텍스트 누락 여부 확인
- 메시지 두 경로에 사용자명과 조사 처리 적용
- ADV 사용자명 getter 재탐색
- iOS 설정 앱에 `displayUserName` 입력 칸 추가
- 일본어 잔존 로그가 있을 때만 `SetUpI18n` 또는 `GetOrDefault`를 추가 조사

완료 조건은 Android와 같은 사용자명 치환 결과가 ADV·메시지·알림에서 나오고, 기존 화면에
회귀가 없는 것이다.

### 2단계: 이미지 교체

- 836개 PNG를 IPA의 `HoshimiLocal/local-files/resource/img`에 패키징
- 네 UI 진입점과 이미지 생성 보조 API 구현
- 원본 스프라이트 형상 값 보존, 이름별 캐시와 실패 캐시 구현
- `replaceImages` 설정을 실제 실행 게이트로 연결

완료 조건은 대표 배너·버튼·튜토리얼 이미지가 바뀌고, 원본 비율·9-slice 이미지가 깨지지
않으며, 설정을 끄고 재실행하면 원본 이미지가 나오는 것이다.

### 3단계: 전화 자막

- 514개 클립/4,681줄을 동일 데이터로 패키징
- 다섯 AudioSource 경로에서 `sud_vo_phone` 클립과 시작 지연을 추적
- main-thread 갱신 진입점과 TMP 자막 표시를 Android 방식으로 구현
- `usePhoneSubtitles` 설정을 실제 실행 게이트로 연결

완료 조건은 일반 재생·지연 재생·OneShot 모두 음성과 자막 시간이 맞고, 통화 종료 시 자막이
사라지며, 설정을 끄면 자막 객체를 남기지 않는 것이다.

### 4단계: 데이터 갱신 기반

- `hoshimi-local` 커밋/데이터 버전을 manifest에 기록
- 새 번역 묶음 검증, 원자적 교체, 이전 버전 복구 구현
- IPA 재설치 없이 번역 데이터만 갱신하는 경로 마련

## 이식하지 않을 Android 기능

다음은 한글패치 기능이 아니므로 이번 iOS 동등성 작업에서 제외한다.

- 런타임 폰트 생성 방식 2 (`useRuntimeKoreanFont`): 사용자가 선택한 정적 교체 방식 1 유지
- FPS·해상도·그래픽 품질·화면 방향 변경
- 자유 카메라와 FOV 조절
- 라이브/의상/사진 기능 해금과 커스텀 의상
- 플랫폼 위장, 물리 설정 변경
- 번역 제작용 텍스트/리소스 덤프와 대량 디버그 로그

각 단계는 별도 버전으로 만들고 iPad에서 성공한 버전을 다음 단계의 기준으로 삼는다. 여러
종류의 새 정적 훅을 한 IPA에 동시에 넣지 않는다.
