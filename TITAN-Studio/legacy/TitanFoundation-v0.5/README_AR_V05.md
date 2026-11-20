# TITAN Engine v0.5 — إضافات أصلية على UE4.27.2

هذا التحديث مبني على حزمة TITAN v0.4 ويضيف **قياس أداء عمليات Streaming**،
مع توافق مصمم حول واجهات UE4.27 التي تمت معاينتها في مستودع المستخدم.

## نتائج المراجعة

المستودع المصدر `AlexMercer-MA/UnrealEngine-4.27` يعرّف نفسه على أنه Unreal Engine 4.27.2.
اتصال GitHub المستخدم للمراجعة هو `geniusa2009-stack` ولا يملك صلاحية Push
على هذا المستودع، لذا **لم تُجرَ أي تعديلات على GitHub**.

## التثبيت من دون الكتابة فوق المحرك

```bash
python3 tools/verify_ue427_source_contract.py --engine /path/to/UnrealEngine-4.27
python3 scripts/install_into_ue427.py --engine /path/to/UnrealEngine-4.27
python3 scripts/install_into_ue427.py --engine /path/to/UnrealEngine-4.27 --apply
```

المثبت يرفض إعادة التثبيت فوق إضافة موجودة. لا تشغّل `Setup.sh` لمستودع غير رسمي
قبل فحص مصدره ومقارنة ملفاته بالمصدر المصرّح به.

## الاختبارات

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

## الاستخدام

ضع `TitanWorldOrchestratorComponent` و`TitanLevelStreamingBridgeComponent`
على Actor واحد في العالم، وسجّل المستويات مسبقًا داخل persistent level
مع أسماء مماثلة لـ `Tile_p0_p0` كما في وثائق v0.4.

في Blueprint استدعِ `GetStreamingCounters` من `TitanLevelStreamingBridgeComponent`.
يظهر عدد التحميلات والتحميلات الفاشلة والـTimeouts، ومتوسط وزمن P95.

النتائج لا تعني أن العوالم أو الـGPU أصبحت أسرع، لكنها أساس ضروري للقياس
قبل اتخاذ قرارات إعادة هندسة الـStreaming في TITAN.
