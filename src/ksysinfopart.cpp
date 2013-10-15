/*  This file is part of the KDE project
    Copyright (C) 2002 Alexander Neundorf <neundorf@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "ksysinfopart.h"

#include <qtimer.h>
#include <QMouseEvent>
#include <kcomponentdata.h>
#include <kglobal.h>
#include <kdebug.h>
#include <klocale.h>
#include <kstandarddirs.h>
#include <kaboutdata.h>
#include <kdeversion.h>
#include <kmenu.h>
#include <khtmlview.h>
#include <khtml_events.h>
#include <qcursor.h>
#include <kio/netaccess.h>
#include <kfileitem.h>
#include <KDesktopFile>

//solid
#include <solid/networking.h>
#include <solid/device.h>
#include <solid/deviceinterface.h>
#include <solid/devicenotifier.h>
#include <solid/predicate.h>
#include <solid/storagedrive.h>
#include <solid/storageaccess.h>
#include <solid/storagevolume.h>
#include <solid/opticaldisc.h>
#include <solid/opticaldrive.h>

extern "C"
{
    KDE_EXPORT void* init_libksysinfopart()
    {
        return new KSysinfoPartFactory;
    }
}

KComponentData* KSysinfoPartFactory::s_componentData = 0L;
KAboutData* KSysinfoPartFactory::s_about = 0L;

KSysinfoPartFactory::KSysinfoPartFactory( QObject* parent )
    : KParts::Factory( parent )
{}

KSysinfoPartFactory::~KSysinfoPartFactory()
{
    delete s_componentData;
    s_componentData = 0L;
    delete s_about;
}

KParts::Part* KSysinfoPartFactory::createPartObject( QWidget * parentWidget, QObject *,
                                                     const char* /*className*/,const QStringList & )
{
    KSysinfoPart* part = new KSysinfoPart(parentWidget);
    return part;
}

KComponentData* KSysinfoPartFactory::instance()
{
    if( !s_componentData )
    {
        s_about = new KAboutData( "ksysinfopart", 0, ki18n("KSysInfo"), KDE_VERSION_STRING,
                                  ki18n( "Embeddable System Information" ), KAboutData::License_GPL );
        s_componentData = new KComponentData( s_about );
    }
    return s_componentData;
}


KSysinfoPart::KSysinfoPart( QWidget * parent )
    : KHTMLPart( parent )
{
    KComponentData * instance = new KComponentData( "ksysinfopart" );
    setComponentData( *instance );
    rescanTimer=new QTimer(this);
    connect(rescanTimer, SIGNAL(timeout()), SLOT(rescan()));
    rescanTimer->setSingleShot(true);
    rescanTimer->start(20000);
    setJScriptEnabled(false);
    setJavaEnabled(false);
    setPluginsEnabled(false);
    setMetaRefreshEnabled(false);

    connect(Solid::Networking::notifier(), SIGNAL(statusChanged(Solid::Networking::Status)),
            this, SLOT(rescan()));
    connect(Solid::DeviceNotifier::instance(), SIGNAL(deviceAdded(const QString &)),
            this, SLOT(onDeviceAdded(const QString &)));
    connect(Solid::DeviceNotifier::instance(), SIGNAL(deviceRemoved(const QString &)),
            this, SLOT(rescan()));

    QList<Solid::Device> deviceList = Solid::Device::listFromQuery("IS StorageAccess");
    Q_FOREACH (const Solid::Device &device, deviceList)
    {
        const Solid::StorageAccess *access = device.as<Solid::StorageAccess>();
        connect(access, SIGNAL(accessibilityChanged(bool, const QString &)),
                this, SLOT(rescan()));
    }

    installEventFilter( this );
}

void KSysinfoPart::slotResult( KJob *job)
{
    KIO::StatJob *sjob = dynamic_cast<KIO::StatJob*>(job);
    if (!job)
        return;

    KFileItemList list;
    list.append(KFileItem(sjob->statResult(), sjob->url()));
    emit browserExtension()->popupMenu(QCursor::pos(), list); //, );
}

void KSysinfoPart::customEvent( QEvent *event )
{
    if ( KParts::Event::test(event, "khtml/Events/MousePressEvent"))
    {
        khtml::MousePressEvent *ev = static_cast<khtml::MousePressEvent *>( event );
        KUrl url(ev->url().string());
        if (url.path().startsWith("/dev/") && (ev->qmouseEvent()->button() & Qt::RightButton))
        {
            KIO::UDSEntry entry;
            KIO::Job *job = KIO::stat(url, KIO::HideProgressInfo);
            connect( job, SIGNAL( result( KJob * ) ),
                     SLOT( slotResult( KJob * ) ) );
            return;
        }
    }

    KHTMLPart::customEvent(event);
}

void KSysinfoPart::mountDevice( Solid::Device *device )
{
    QStringList interestingDesktopFiles;
    foreach (const QString &path, KGlobal::dirs()->findAllResources("data", "solid/actions/")) {
        KDesktopFile cfg(path);
        const QString string_predicate = cfg.desktopGroup().readEntry("X-KDE-Solid-Predicate");
        QString fileUrl = KUrl(path).fileName();
        Solid::Predicate predicate = Solid::Predicate::fromString(string_predicate);
        if (predicate.matches(*device)) {
            interestingDesktopFiles << fileUrl;
        }
    }
    QDBusInterface soliduiserver("org.kde.kded", "/modules/soliduiserver", "org.kde.SolidUiServer");
    QDBusReply<void> reply = soliduiserver.call("showActionsDialog", device->udi(), interestingDesktopFiles);
}

bool KSysinfoPart::unmountDevice( Solid::Device *device )
{
    if (device->is<Solid::OpticalDisc>()) {
        Solid::OpticalDrive *drive = device->parent().as<Solid::OpticalDrive>();
        if (drive) {
            drive->eject();
        }
    }
    else if (device->is<Solid::StorageVolume>()) {
        Solid::StorageAccess *access = device->as<Solid::StorageAccess>();
        if (access && access->isAccessible()) {
            access->teardown();
            return true;
        }
    }

    return false;
}

bool KSysinfoPart::urlSelected( const QString &url,
                                int button,
                                int state,
                                const QString &_target,
                                const KParts::OpenUrlArguments& _args,
                                const KParts::BrowserArguments& _browserArgs)
{
    if (url.startsWith("#unmount=")) {
        Solid::Device device = Solid::Device(url.mid(9));
        if (!device.isValid()) {
            kDebug(1242) << "Device for udi" << device.udi() << "is invalid";
            return false;
        }
        return unmountDevice(&device);
    }

    QString path = KUrl(url).path();
    if (path.startsWith("/dev/")) {
        Solid::Predicate predicate(Solid::DeviceInterface::Block, "device", path);
        QList<Solid::Device> devList = Solid::Device::listFromQuery(predicate, QString());

        if (!devList.empty()) {
            Solid::StorageAccess *access = devList[0].as<Solid::StorageAccess>();
            if (access && access->isAccessible()) { //mounted
                openUrl(KUrl(access->filePath()));
                return true;
            }
            else { //not mounted
                Solid::Device device = Solid::Device(devList[0]);
                Solid::StorageDrive *drive = device.parent().as<Solid::StorageDrive>();
                if ((drive && drive->isHotpluggable()) || device.is<Solid::OpticalDisc>()) {
                    mountDevice(&device);
                    return true;
                }
                kDebug(1242) << "Device" << path << "not mounted and not hotpluggable";
                return false;
            }
        }
        else {
            kDebug(1242) << "Device" << path << "not found";
            return false;
        }
    }

    return KHTMLPart::urlSelected(url, button, state, _target, _args, _browserArgs);
}

void KSysinfoPart::onDeviceAdded(const QString &udi) {
    Solid::Device device = Solid::Device(udi);
    Solid::StorageAccess *access = device.as<Solid::StorageAccess>();
    if (access) {
        connect(access, SIGNAL(accessibilityChanged(bool, const QString &)),
                this, SLOT(rescan()));
    }
    rescan();
}

void KSysinfoPart::rescan()
{
    openUrl(KUrl("sysinfo:/"));
    rescanTimer->stop();
    rescanTimer->start(20000);
}

#ifdef PORTED
void KSysinfoPart::FilesAdded( const KUrl & dir )
{
    if (dir.protocol() == "media")
    {
        rescanTimer->stop();
        rescanTimer->start(10, true);
    }
}

void KSysinfoPart::FilesRemoved( const KUrl::List & urls )
{
    for ( KUrl::List::ConstIterator it = urls.begin() ; it != urls.end() ; ++it )
        FilesAdded( *it );
}

void KSysinfoPart::FilesChanged( const KUrl::List & urls )
{
    // not same signal, but same implementation
    FilesRemoved( urls );
}
#endif

#include "ksysinfopart.moc"

