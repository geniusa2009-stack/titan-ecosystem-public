# TITAN v0.5 — فحص المصدر الحقيقي وخطة التكامل

**المصدر:** https://github.com/AlexMercer-MA/UnrealEngine-4.27/tree/master

**بناءً على قراءة مباشرة عبر موصل GitHub:**
- `Engine/Build/Build.version`: `4.27.2`, `BranchName = ++UE4+Release-4.27`.
- `Engine/Source/Runtime`: مجلدات runtime متنوعة تشمل Core, CoreUObject, Engine, AnimationCore, RenderCore وغيرها.
- `Engine/Source/Runtime/Engine/Classes/Engine/LevelStreaming.h`: الواجهات `SetShouldBeLoaded`, `SetShouldBeVisible`, `IsLevelLoaded`, `IsLevelVisible`.
- `Engine/Source/Runtime/Engine/Classes/Kismet/GameplayStatics.h`: الواجهة `GetStreamingLevel`.
- `Engine/Source/Runtime/Engine/Classes/Engine/World.h`: `GetStreamingLevels`.
- `LICENSE.md`: يشير إلى اتفاقية Unreal Engine EULA ولا يعلن MIT أو Apache.

## التغيير الجديد

تم تطوير `TitanStreamingTelemetry` في نواة `TitanOrchestrationCore` لتجميع إحصاءات
التحميل والتفريغ. التتبع مرتبط الآن بـ`UTitanLevelStreamingBridgeComponent`؛
يمكن الاستعلام من Blueprint عبر `GetStreamingCounters()` عن عدد المحاولات،
النجاحات، حالات الفشل، انتهاء المهلة، P95 ومتوسط زمن التحميل.

مدة العملية محسوبة من لحظة الطلب حتى ملاحظة الحالة داخل Tick. لا تمثل زمن
التحميل الداخلي للقرص أو GPU، ولا تُظهر حجم الذاكرة الفعلي.

## التحقق

- CMake/CTest لتشغيل اختبارات النواة المستقلة على Linux/macOS.
- Python Unit Tests للتحقق من تثبيت الإضافة وعدم الكتابة فوق ملفات Epic.
- `tools/verify_ue427_source_contract.py --engine ...`: فحص إصدار المحرك وواجهات C++ المتوقعة في Checkout محلي.

**حدود:** لم يُشغَّل UnrealBuildTool ولم يُفتح المحرر في بيئة الإنشاء، لذلك
التوافق الفعلي للإضافة داخل UE4.27 لم يتم إثباته بعد.

## حقوق المصدر

هذه الحزمة تضيف فقط كود TITAN المكتوب حديثًا والإصدارات السابقة من
وحداته، ولا تتضمن كود Epic. امتلاك الوصول إلى مستودع عام أو ترخيص تشغيل
لا يثبت تلقائيًا السماح بالتوزيع التجاري لمحرك مشتق؛ راجع الاتفاقية الموقعة.
