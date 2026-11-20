# TITAN × المستودع الأصلي UE4.27.2 — تكامل مباشر

**المصدر الذي فحصناه:** `AlexMercer-MA/UnrealEngine-4.27`, فرع `master` (كان آخر Push بتاريخ 2024-07-29 عند الفحص 2026-11-05).

**النسخة المؤكدة:** `Engine/Build/Build.version` يذكر `4.27.2`، وفرع `++UE4+Release-4.27`.

## ما الذي تحققنا منه من كود المستودع؟

- `Engine/Source/Runtime/Engine/Classes/Engine/LevelStreaming.h` يحتوي `SetShouldBeLoaded`, `SetShouldBeVisible`, `IsLevelLoaded`, `IsLevelVisible`.
- `Engine/Source/Runtime/Engine/Classes/Kismet/GameplayStatics.h` يحتوي `GetStreamingLevel`.
- `Engine/Source/Runtime/Engine/Private/World.cpp` مسؤول عن عناصر مهمة من العالم؛ لا يحتاج TITAN إلى تعديل هذا الملف الآن.
- `Engine/Source/Runtime` فيه أنظمة مختلفة، منها Core وEngine وAnimationCore وAIModule.

## ما الذي أضفناه الآن؟

ملف `TitanLevelStreamingBridgeComponent` يُوصل طلبات `TitanWorldOrchestrator` مباشرة بكائنات `ULevelStreaming` الأصلية، ويتابع التحميل/التفريغ حتى الاكتمال أو المهلة قبل إرسال ACK إلى المجدول. لا يعتبر مجرد إصدار الطلب نجاحًا. وهو لا يقوم بإنشاء مستويات جديدة آليًا: يجب إعداد Sublevels في Persistent Level.

## طريقة الدمج (بدون تعديل كود Unreal الأصلي)

1. على نسخة محلية تملك حق الوصول إليها، خذ نسخة احتياطية أو أنشئ فرع Git جديد.
2. من مجلد الحزمة، نفذ فحصًا أوليًا بدون تعديل:

```bash
python3 scripts/install_into_ue427.py --engine /path/to/UnrealEngine-4.27
```

3. للنسخ الفعلي إلى `Engine/Plugins/Runtime/TitanFoundation`:

```bash
python3 scripts/install_into_ue427.py --engine /path/to/UnrealEngine-4.27 --apply
```

4. افتح مشروع UE4.27 C++ مُكوّنًا للعمل على نسخة المحرك، وفعّل `TitanFoundation` من Plugins.
5. في المستوى الدائم Persistent Level، أضف Sublevels بالأسماء `Tile_p0_p0`, `Tile_p1_p0`, `Tile_m1_p0` ... إلخ. (p غير سالب، m سالب) واستخدم Streamed levels.
6. على ممثل Actor واحد، أضف مكون `TitanWorldOrchestratorComponent` ومكون `TitanLevelStreamingBridgeComponent`، واضبط `TrackingActor` ومعدل الطلبات ونصف القطر حسب حجم المشهد.
7. العب داخل محرر UE ولاحظ مستويات التحميل من World Outliner، وسجّل الأداء بوسائل Unreal، ثم تحقّق من الـACK وعدادات المجدول.

## حدود النسخة

- **لم يُنفذ Build في بيئة Unreal**، ولم يتم اختبار التشغيل على Metal/macOS. اختبارات C++ المحمولة وPython ليست إثباتًا لنجاح Build داخل Unreal.
- الـStreaming هنا يستخدم Sublevels الموجودة مسبقًا، وليس World Partition الخاص بـUE5، ولا نظام تجزئة هندسي شبيه Nanite.
- `EstimatedMegabytes` تقدير لميزانية الخلية، وليس ذاكرة مقاسة لكل مستوى؛ لا تعتبره RAM Usage مؤكدة.
- فشل تحميل خلية غير مسجلة يؤدي إلى ACK فشل وقد تتكرر الطلبات. ضع مجموعة Sublevels مناسبة للـLoadRadius أو طور سياسة مناطق صالحة لاحقًا.
- الـPlugin يضيف كود TITAN فقط؛ لا ينقل الملفات المحمية في مستودع Epic إلى حزمة منفصلة.

## مصدر الحقوق

هذه الحزمة لا تتحقق من اتفاقية الترخيص الخاصة التي ذكرت حصولك عليها. نسخ المصدر المحلي أو تعديله يختلف عن إعادة توزيعه للمستخدمين أو النشر التجاري. احتفظ بالاتفاقية قبل نشر محرك مشتق، وراجع حقوق طرف ثالث داخل المستودع.
