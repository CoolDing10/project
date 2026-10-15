/****************************************************************************
** Meta object code from reading C++ file 'archive_browser.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.12.9)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../pc_receiver/archive_browser.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'archive_browser.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.12.9. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ArchiveBrowser_t {
    QByteArrayData data[9];
    char stringdata0[132];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ArchiveBrowser_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ArchiveBrowser_t qt_meta_stringdata_ArchiveBrowser = {
    {
QT_MOC_LITERAL(0, 0, 14), // "ArchiveBrowser"
QT_MOC_LITERAL(1, 15, 14), // "refreshArchive"
QT_MOC_LITERAL(2, 30, 0), // ""
QT_MOC_LITERAL(3, 31, 16), // "downloadSelected"
QT_MOC_LITERAL(4, 48, 11), // "sendRequest"
QT_MOC_LITERAL(5, 60, 12), // "readResponse"
QT_MOC_LITERAL(6, 73, 17), // "handleSocketError"
QT_MOC_LITERAL(7, 91, 18), // "showNextMjpegFrame"
QT_MOC_LITERAL(8, 110, 21) // "openDownloadDirectory"

    },
    "ArchiveBrowser\0refreshArchive\0\0"
    "downloadSelected\0sendRequest\0readResponse\0"
    "handleSocketError\0showNextMjpegFrame\0"
    "openDownloadDirectory"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ArchiveBrowser[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   49,    2, 0x0a /* Public */,
       3,    0,   50,    2, 0x08 /* Private */,
       4,    0,   51,    2, 0x08 /* Private */,
       5,    0,   52,    2, 0x08 /* Private */,
       6,    0,   53,    2, 0x08 /* Private */,
       7,    0,   54,    2, 0x08 /* Private */,
       8,    0,   55,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void ArchiveBrowser::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ArchiveBrowser *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->refreshArchive(); break;
        case 1: _t->downloadSelected(); break;
        case 2: _t->sendRequest(); break;
        case 3: _t->readResponse(); break;
        case 4: _t->handleSocketError(); break;
        case 5: _t->showNextMjpegFrame(); break;
        case 6: _t->openDownloadDirectory(); break;
        default: ;
        }
    }
    Q_UNUSED(_a);
}

QT_INIT_METAOBJECT const QMetaObject ArchiveBrowser::staticMetaObject = { {
    &QWidget::staticMetaObject,
    qt_meta_stringdata_ArchiveBrowser.data,
    qt_meta_data_ArchiveBrowser,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ArchiveBrowser::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ArchiveBrowser::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ArchiveBrowser.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int ArchiveBrowser::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 7;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
