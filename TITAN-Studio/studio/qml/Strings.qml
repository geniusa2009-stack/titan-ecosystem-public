// TITAN Studio — UI string table (English source, Arabic translation). Technical identifiers,
// numbers and code stay LTR. Original TITAN content.
pragma Singleton
import QtQuick

QtObject {
    property string lang: "en"
    readonly property bool rtl: lang === "ar"

    readonly property var ar: ({
        "New Project…": "مشروع جديد…",
        "Open Project…": "فتح مشروع…",
        "Recent Projects": "المشاريع الأخيرة",
        "No recent projects yet.": "لا توجد مشاريع حديثة بعد.",
        "Create a project to get started.": "أنشئ مشروعًا للبدء.",
        "Missing": "مفقود",
        "Remove": "إزالة",
        "Appearance": "المظهر",
        "Language": "اللغة",
        "System": "النظام",
        "Light": "فاتح",
        "Dark": "داكن",
        "World": "العالم",
        "Code": "الشيفرة",
        "Materials": "المواد",
        "Cinematic": "السينمائي",
        "Performance": "الأداء",
        "AI Lab": "مختبر الذكاء",
        "Source Engineering": "هندسة المصادر",
        "Extensions": "الإضافات",
        "Scene": "المشهد",
        "Search": "بحث",
        "Inspector": "المفتش",
        "World Builder": "باني العالم",
        "No selection": "لا يوجد تحديد",
        "Select an object in the viewport or the scene list.": "حدّد كائنًا في نافذة العرض أو قائمة المشهد.",
        "Name": "الاسم",
        "Transform": "التحويل",
        "Position": "الموضع",
        "Rotation": "الدوران",
        "Scale": "الحجم",
        "Appearance ": "المظهر",
        "Color": "اللون",
        "Visible": "مرئي",
        "Light ": "الإضاءة",
        "Intensity": "الشدة",
        "Save": "حفظ",
        "Undo": "تراجع",
        "Redo": "إعادة",
        "Saved": "محفوظ",
        "Unsaved changes": "تغييرات غير محفوظة",
        "Connected": "متصل",
        "Preview": "معاينة",
        "Approve": "اعتماد",
        "Discard": "تجاهل",
        "New recipe": "وصفة جديدة",
        "Add": "إضافة",
        "Delete": "حذف",
        "Frame Selection": "تأطير التحديد",
        "Reset Camera": "إعادة ضبط الكاميرا",
        "Close Project": "إغلاق المشروع",
        "Restart Engine": "إعادة تشغيل المحرك",
        "Engine": "المحرك",
        "TITAN Reference": "TITAN المرجعي",
        "Unreal Engine 5.8.4": "Unreal Engine 5.8.4",
        "Unreal Editor is not built yet (scripts/ue_build.sh build).": "لم يتم بناء محرر Unreal بعد (scripts/ue_build.sh build).",
        "Show Unreal Editor Window": "إظهار نافذة محرر Unreal",
        "Hide Unreal Editor Window": "إخفاء نافذة محرر Unreal",
        "Toggle Light/Dark": "تبديل فاتح/داكن",
        "Command Palette": "لوحة الأوامر",
        "Type a command…": "اكتب أمرًا…",
        "Create": "إنشاء",
        "Cancel": "إلغاء",
        "Project name": "اسم المشروع",
        "Location": "الموقع",
        "Choose…": "اختيار…",
        "Planned — not available in TITAN Studio Alpha": "مخطط — غير متاح في إصدار ألفا",
        "Live rendering": "عرض مستمر",
        "Don't Save": "عدم الحفظ",
        "Save changes before closing?": "حفظ التغييرات قبل الإغلاق؟"
    })

    function t(s) {
        if (lang === "ar" && ar[s] !== undefined) return ar[s]
        return s.trim()
    }
}
