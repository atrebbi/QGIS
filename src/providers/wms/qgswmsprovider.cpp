/***************************************************************************
  qgswmsprovider.cpp  -  QGIS Data provider for
                         OGC Web Map Service layers
                             -------------------
    begin                : 17 Mar, 2005
    copyright            : (C) 2005 by Brendan Morley
    email                : morb at ozemail dot com dot au

    wms-c/wmts support   : Jürgen E. Fischer < jef at norbit dot de >, norBIT GmbH

    tile retry support   : Luigi Pirelli < luipir at gmail dot com >
                           (funded by Regione Toscana-SITA)

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <typeinfo>

// time to wait for an answer without emitting dataChanged()
#define WMS_THRESHOLD 200

#include "qgslogger.h"
#include "qgswmsprovider.h"
#include "qgswmsconnection.h"
#include "qgscoordinatetransform.h"
#include "qgsdatasourceuri.h"
#include "qgsfeaturestore.h"
#include "qgsrasteridentifyresult.h"
#include "qgsrasterlayer.h"
#include "qgsrectangle.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsmessageoutput.h"
#include "qgsmessagelog.h"
#include "qgsnetworkaccessmanager.h"
#include "qgsnetworkreplyparser.h"
#include "qgsgml.h"
#include "qgsgmlschema.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkDiskCache>

#include <QtXmlPatterns/QXmlSchema>
#include <QtXmlPatterns/QXmlSchemaValidator>

#include <QUrl>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QEventLoop>
#include <QCoreApplication>
#include <QTextCodec>
#include <QTime>

#ifdef QGISDEBUG
#include <QFile>
#include <QDir>
#endif

#define ERR(message) QGS_ERROR_MESSAGE(message,"WMS provider")
#define SRVERR(message) QGS_ERROR_MESSAGE(message,"WMS server")
#define ERROR(message) QgsError(message,"WMS provider")

static QString WMS_KEY = "wms";
static QString WMS_DESCRIPTION = "OGC Web Map Service version 1.3 data provider";

static QString DEFAULT_LATLON_CRS = "CRS:84";

QgsWmsProvider::QgsWmsProvider( QString const &uri )
    : QgsRasterDataProvider( uri )
    , mHttpUri( uri )
    , mHttpCapabilitiesResponse( 0 )
    , mHttpGetLegendGraphicResponse( 0 )
    , mGetLegendGraphicImage()
    , mGetLegendGraphicScale( 0.0 )
    , mImageCrs( DEFAULT_LATLON_CRS )
    , mCachedImage( 0 )
    , mCacheReply( 0 )
    , mCachedViewExtent( 0 )
    , mCoordinateTransform( 0 )
    , mExtentDirty( true )
    , mGetFeatureInfoUrlBase( "" )
    , mLayerCount( -1 )
    , mTileReqNo( 0 )
    , mCacheHits( 0 )
    , mCacheMisses( 0 )
    , mErrors( 0 )
    , mUserName( QString::null )
    , mPassword( QString::null )
    , mReferer( QString::null )
    , mTiled( false )
    , mTileLayer( 0 )
    , mTileMatrixSetId( QString::null )
    , mTileMatrixSet( 0 )
    , mFeatureCount( 0 )
{
  QgsDebugMsg( "constructing with uri '" + mHttpUri + "'." );

  mSupportedGetFeatureFormats = QStringList() << "text/html" << "text/plain" << "text/xml" << "application/vnd.ogc.gml";

  mValid = false;

  // URL may contain username/password information for a WMS
  // requiring authentication. In this case the URL is prefixed
  // with username=user,password=pass,url=http://xxx.xxx.xx/yyy...
  if ( !parseUri( uri ) )
  {
    appendError( ERR( tr( "Cannot parse URI" ) ) );
    return;
  }

  if ( !calculateExtent() || mLayerExtent.isEmpty() )
  {
    appendError( ERR( tr( "Cannot calculate extent" ) ) );
    return;
  }

  // URL can be in 3 forms:
  // 1) http://xxx.xxx.xx/yyy/yyy
  // 2) http://xxx.xxx.xx/yyy/yyy?
  // 3) http://xxx.xxx.xx/yyy/yyy?zzz=www

  mValid = true;
  QgsDebugMsg( "exiting constructor." );
}

bool QgsWmsProvider::parseUri( QString uriString )
{
  QgsDebugMsg( "uriString = " + uriString );
  QgsDataSourceURI uri;
  uri.setEncodedUri( uriString );

  mTiled = false;
  mTileMatrixSet = 0;
  mTileLayer = 0;
  mTileDimensionValues.clear();

  mMaxWidth = 0;
  mMaxHeight = 0;

  mHttpUri = uri.param( "url" );
  mBaseUrl = prepareUri( mHttpUri ); // must set here, setImageCrs is using that
  QgsDebugMsg( "mBaseUrl = " + mBaseUrl );

  mIgnoreGetMapUrl = uri.hasParam( "IgnoreGetMapUrl" );
  mIgnoreGetFeatureInfoUrl = uri.hasParam( "IgnoreGetFeatureInfoUrl" );
  mIgnoreAxisOrientation = uri.hasParam( "IgnoreAxisOrientation" ); // must be before parsing!
  mInvertAxisOrientation = uri.hasParam( "InvertAxisOrientation" ); // must be before parsing!
  mSmoothPixmapTransform = uri.hasParam( "SmoothPixmapTransform" );

  mDpiMode = uri.hasParam( "dpiMode" ) ? ( QgsWmsDpiMode ) uri.param( "dpiMode" ).toInt() : dpiAll;

  mUserName = uri.param( "username" );
  QgsDebugMsg( "set username to " + mUserName );

  mPassword = uri.param( "password" );
  QgsDebugMsg( "set password to " + mPassword );

  mReferer = uri.param( "referer" );
  QgsDebugMsg( "set referer to " + mReferer );

  addLayers( uri.params( "layers" ), uri.params( "styles" ) );
  setImageEncoding( uri.param( "format" ) );

  if ( uri.hasParam( "maxWidth" ) && uri.hasParam( "maxHeight" ) )
  {
    mMaxWidth = uri.param( "maxWidth" ).toInt();
    mMaxHeight = uri.param( "maxHeight" ).toInt();
  }

  if ( uri.hasParam( "tileMatrixSet" ) )
  {
    mTiled = true;
    // tileMatrixSet may be empty if URI was converted from < 1.9 project file URI
    // in that case it means that the source is WMS-C
    mTileMatrixSetId = uri.param( "tileMatrixSet" );
  }

  if ( uri.hasParam( "tileDimensions" ) )
  {
    mTiled = true;
    foreach ( QString param, uri.param( "tileDimensions" ).split( ";" ) )
    {
      QStringList kv = param.split( "=" );
      if ( kv.size() == 1 )
      {
        mTileDimensionValues.insert( kv[0], QString::null );
      }
      else if ( kv.size() == 2 )
      {
        mTileDimensionValues.insert( kv[0], kv[1] );
      }
      else
      {
        QgsDebugMsg( QString( "skipped dimension %1" ).arg( param ) );
      }
    }
  }

  // setImageCrs is using mTiled !!!
  if ( !setImageCrs( uri.param( "crs" ) ) )
  {
    appendError( ERR( tr( "Cannot set CRS" ) ) );
    return false;
  }
  mCrs.createFromOgcWmsCrs( uri.param( "crs" ) );

  mFeatureCount = uri.param( "featureCount" ).toInt(); // default to 0

  return true;
}

QString QgsWmsProvider::prepareUri( QString uri ) const
{
  if ( uri.contains( "SERVICE=WMTS" ) || uri.contains( "/WMTSCapabilities.xml" ) )
  {
    return uri;
  }

  if ( !uri.contains( "?" ) )
  {
    uri.append( "?" );
  }
  else if ( uri.right( 1 ) != "?" && uri.right( 1 ) != "&" )
  {
    uri.append( "&" );
  }

  return uri;
}

QgsWmsProvider::~QgsWmsProvider()
{
  QgsDebugMsg( "deconstructing." );

  // Dispose of any cached image as created by draw()
  if ( mCachedImage )
  {
    delete mCachedImage;
    mCachedImage = 0;
  }

  if ( mCoordinateTransform )
  {
    delete mCoordinateTransform;
    mCoordinateTransform = 0;
  }

  if ( mCacheReply )
  {
    mCacheReply->deleteLater();
    mCacheReply = 0;
  }

  while ( !mTileReplies.isEmpty() )
  {
    mTileReplies.takeFirst()->deleteLater();
  }
}

QgsRasterInterface * QgsWmsProvider::clone() const
{
  QgsWmsProvider * provider = new QgsWmsProvider( dataSourceUri() );
  return provider;
}

bool QgsWmsProvider::supportedLayers( QVector<QgsWmsLayerProperty> &layers )
{
  QgsDebugMsg( "Entering." );

  // Allow the provider to collect the capabilities first.
  if ( !retrieveServerCapabilities() )
  {
    return false;
  }

  layers = mLayersSupported;

  QgsDebugMsg( "Exiting." );

  return true;
}

bool QgsWmsProvider::supportedTileLayers( QList<QgsWmtsTileLayer> &layers )
{
  QgsDebugMsg( "Entering." );

  // Allow the provider to collect the capabilities first.
  if ( !retrieveServerCapabilities() )
  {
    return false;
  }

  layers = mTileLayersSupported;

  QgsDebugMsg( "Exiting." );

  return true;
}

bool QgsWmsProvider::supportedTileMatrixSets( QHash<QString, QgsWmtsTileMatrixSet> &tileMatrixSets )
{
  QgsDebugMsg( "Entering." );

  // Allow the provider to collect the capabilities first.
  if ( !retrieveServerCapabilities() )
  {
    return false;
  }

  tileMatrixSets = mTileMatrixSets;

  QgsDebugMsg( "Exiting." );

  return true;
}

size_t QgsWmsProvider::layerCount() const
{
  return 1;                   // XXX properly return actual number of layers
} // QgsWmsProvider::layerCount()

QString QgsWmsProvider::baseUrl() const
{
  return mBaseUrl;
}

QString QgsWmsProvider::getMapUrl() const
{
  return mCapabilities.capability.request.getMap.dcpType.size() == 0
         ? mBaseUrl
         : prepareUri( mCapabilities.capability.request.getMap.dcpType.front().http.get.onlineResource.xlinkHref );
}


QString QgsWmsProvider::getFeatureInfoUrl() const
{
  return mCapabilities.capability.request.getFeatureInfo.dcpType.size() == 0
         ? mBaseUrl
         : prepareUri( mCapabilities.capability.request.getFeatureInfo.dcpType.front().http.get.onlineResource.xlinkHref );
}

QString QgsWmsProvider::getTileUrl() const
{
  if ( mCapabilities.capability.request.getTile.dcpType.size() == 0 ||
       ( mCapabilities.capability.request.getTile.allowedEncodings.size() > 0 &&
         !mCapabilities.capability.request.getTile.allowedEncodings.contains( "KVP" ) ) )
  {
    return QString::null;
  }
  else
  {
    return prepareUri( mCapabilities.capability.request.getTile.dcpType.front().http.get.onlineResource.xlinkHref );
  }
}

QString QgsWmsProvider::getLegendGraphicUrl() const
{
  QString url;

  for ( int i = 0; i < mLayersSupported.size() && url.isEmpty(); i++ )
  {
    const QgsWmsLayerProperty &l = mLayersSupported[i];

    if ( l.name != mActiveSubLayers[0] )
      continue;

    for ( int j = 0; j < l.style.size() && url.isEmpty(); j++ )
    {
      const QgsWmsStyleProperty &s = l.style[j];

      if ( s.name != mActiveSubStyles[0] )
        continue;

      for ( int k = 0; k < s.legendUrl.size() && url.isEmpty(); k++ )
      {
        const QgsWmsLegendUrlProperty &l = s.legendUrl[k];

        if ( l.format != mImageMimeType )
          continue;

        url = l.onlineResource.xlinkHref;
      }
    }
  }

  if ( url.isEmpty() && mCapabilities.capability.request.getLegendGraphic.dcpType.size() > 0 )
  {
    url = mCapabilities.capability.request.getLegendGraphic.dcpType.front().http.get.onlineResource.xlinkHref;
  }

  return url.isEmpty() ? url : prepareUri( url );
}

void QgsWmsProvider::addLayers( QStringList const &layers,
                                QStringList const &styles )
{
  QgsDebugMsg( "Entering: layers:" + layers.join( ", " ) + ", styles:" + styles.join( ", " ) );

  if ( layers.size() != styles.size() )
  {
    QgsMessageLog::logMessage( tr( "Number of layers and styles don't match" ), tr( "WMS" ) );
    mValid = false;
    return;
  }

  // TODO: Make mActiveSubLayers a std::map in order to avoid duplicates
  mActiveSubLayers += layers;
  mActiveSubStyles += styles;

  // Set the visibility of these new layers on by default
  foreach ( const QString &layer, layers )
  {
    mActiveSubLayerVisibility[ layer ] = true;
    QgsDebugMsg( "set visibility of layer '" + layer + "' to true." );
  }

  // now that the layers have changed, the extent will as well.
  mExtentDirty = true;

  if ( mTiled )
    mTileLayer = 0;

  QgsDebugMsg( "Exiting." );
}

void QgsWmsProvider::setConnectionName( QString const &connName )
{
  mConnectionName = connName;
}

void QgsWmsProvider::setLayerOrder( QStringList const &layers )
{
  QgsDebugMsg( "Entering." );

  if ( layers.size() != mActiveSubLayers.size() )
  {
    QgsDebugMsg( "Invalid layer list length" );
    return;
  }

  QMap<QString, QString> styleMap;
  for ( int i = 0; i < mActiveSubLayers.size(); i++ )
  {
    styleMap.insert( mActiveSubLayers[i], mActiveSubStyles[i] );
  }

  for ( int i = 0; i < layers.size(); i++ )
  {
    if ( !styleMap.contains( layers[i] ) )
    {
      QgsDebugMsg( QString( "Layer %1 not found" ).arg( layers[i] ) );
      return;
    }
  }

  mActiveSubLayers = layers;
  mActiveSubStyles.clear();
  for ( int i = 0; i < layers.size(); i++ )
  {
    mActiveSubStyles.append( styleMap[ layers[i] ] );
  }

  QgsDebugMsg( "Exiting." );
}


void QgsWmsProvider::setSubLayerVisibility( QString const & name, bool vis )
{
  if ( !mActiveSubLayerVisibility.contains( name ) )
  {
    QgsDebugMsg( QString( "Layer %1 not found." ).arg( name ) );
    return;
  }

  mActiveSubLayerVisibility[name] = vis;
}


QString QgsWmsProvider::imageEncoding() const
{
  return mImageMimeType;
}


void QgsWmsProvider::setImageEncoding( QString const & mimeType )
{
  QgsDebugMsg( "Setting image encoding to " + mimeType + "." );
  mImageMimeType = mimeType;
}


bool QgsWmsProvider::setImageCrs( QString const & crs )
{
  QgsDebugMsg( "Setting image CRS to " + crs + "." );

  if ( crs != mImageCrs && !crs.isEmpty() )
  {
    // delete old coordinate transform as it is no longer valid
    if ( mCoordinateTransform )
    {
      delete mCoordinateTransform;
      mCoordinateTransform = 0;
    }

    mExtentDirty = true;

    mImageCrs = crs;
  }

  if ( mTiled )
  {
    if ( mActiveSubLayers.size() != 1 )
    {
      appendError( ERR( tr( "Number of tile layers must be one" ) ) );
      return false;
    }

    if ( !retrieveServerCapabilities() )
    {
      // Error set in retrieveServerCapabilities()
      return false;
    }
    QgsDebugMsg( QString( "mTileLayersSupported.size() = %1" ).arg( mTileLayersSupported.size() ) );
    if ( mTileLayersSupported.size() == 0 )
    {
      appendError( ERR( tr( "Tile layer not found" ) ) );
      return false;
    }

    for ( int i = 0; i < mTileLayersSupported.size(); i++ )
    {
      QgsWmtsTileLayer *tl = &mTileLayersSupported[i];

      if ( tl->identifier != mActiveSubLayers[0] )
        continue;

      if ( mTileMatrixSetId.isEmpty() && tl->setLinks.size() == 1 )
      {
        QString tms = tl->setLinks.keys()[0];

        if ( !mTileMatrixSets.contains( tms ) )
        {
          QgsDebugMsg( QString( "tile matrix set '%1' not found." ).arg( tms ) );
          continue;
        }

        if ( mTileMatrixSets[ tms ].crs != mImageCrs )
        {
          QgsDebugMsg( QString( "tile matrix set '%1' has crs %2 instead of %3." ).arg( tms ).arg( mTileMatrixSets[ tms ].crs ).arg( mImageCrs ) );
          continue;
        }

        // fill in generate matrix for WMS-C
        mTileMatrixSetId = tms;
      }

      mTileLayer = tl;
      break;
    }

    QList<QVariant> resolutions;
    if ( mTileMatrixSets.contains( mTileMatrixSetId ) )
    {
      mTileMatrixSet = &mTileMatrixSets[ mTileMatrixSetId ];
      QList<double> keys = mTileMatrixSet->tileMatrices.keys();
      qSort( keys );
      foreach ( double key, keys )
      {
        resolutions << key;
      }
    }
    else
    {
      QgsDebugMsg( QString( "Expected tile matrix set '%1' not found." ).arg( mTileMatrixSetId ) );
      mTileMatrixSet = 0;
    }

    setProperty( "resolutions", resolutions );

    if ( mTileLayer == 0 || mTileMatrixSet == 0 )
    {
      appendError( ERR( tr( "Tile layer or tile matrix set not found" ) ) );
      return false;
    }
  }
  return true;
}

void QgsWmsProvider::setQueryItem( QUrl &url, QString item, QString value )
{
  url.removeQueryItem( item );
  url.addQueryItem( item, value );
}

QImage *QgsWmsProvider::draw( QgsRectangle  const &viewExtent, int pixelWidth, int pixelHeight )
{
  QgsDebugMsg( "Entering." );

  if ( !retrieveServerCapabilities() )
  {
    return 0;
  }

  // Can we reuse the previously cached image?
  if ( mCachedImage &&
       mCachedViewExtent == viewExtent &&
       mCachedViewWidth == pixelWidth &&
       mCachedViewHeight == pixelHeight )
  {
    return mCachedImage;
  }

  // delete cached image and create network request(s) to fill it
  if ( mCachedImage )
  {
    delete mCachedImage;
    mCachedImage = 0;
  }

  // abort running (untiled) request
  if ( mCacheReply )
  {
    mCacheReply->abort();
    delete mCacheReply;
    mCacheReply = 0;
  }

  //according to the WMS spec for 1.3, some CRS have inverted axis
  bool changeXY = false;
  if ( !mIgnoreAxisOrientation && ( mCapabilities.version == "1.3.0" || mCapabilities.version == "1.3" ) )
  {
    //create CRS from string
    QgsCoordinateReferenceSystem theSrs;
    if ( theSrs.createFromOgcWmsCrs( mImageCrs ) && theSrs.axisInverted() )
    {
      changeXY = true;
    }
  }

  if ( mInvertAxisOrientation )
    changeXY = !changeXY;

  // compose the URL query string for the WMS server.
  QString crsKey = "SRS"; //SRS in 1.1.1 and CRS in 1.3.0
  if ( mCapabilities.version == "1.3.0" || mCapabilities.version == "1.3" )
  {
    crsKey = "CRS";
  }

  // Bounding box in WMS format (Warning: does not work with scientific notation)
  QString bbox = QString( changeXY ? "%2,%1,%4,%3" : "%1,%2,%3,%4" )
                 .arg( qgsDoubleToString( viewExtent.xMinimum() ) )
                 .arg( qgsDoubleToString( viewExtent.yMinimum() ) )
                 .arg( qgsDoubleToString( viewExtent.xMaximum() ) )
                 .arg( qgsDoubleToString( viewExtent.yMaximum() ) );

  mCachedImage = new QImage( pixelWidth, pixelHeight, QImage::Format_ARGB32 );
  mCachedImage->fill( 0 );
  mCachedViewExtent = viewExtent;
  mCachedViewWidth = pixelWidth;
  mCachedViewHeight = pixelHeight;

  QSettings s;
  bool bkLayerCaching = s.value( "/qgis/enable_render_caching", false ).toBool();

  if ( !mTiled && !mMaxWidth && !mMaxHeight )
  {
    // Calculate active layers that are also visible.

    QgsDebugMsg( "Active layer list of "  + mActiveSubLayers.join( ", " )
                 + " and style list of "  + mActiveSubStyles.join( ", " ) );

    QStringList visibleLayers = QStringList();
    QStringList visibleStyles = QStringList();

    QStringList::Iterator it2  = mActiveSubStyles.begin();

    for ( QStringList::Iterator it = mActiveSubLayers.begin();
          it != mActiveSubLayers.end();
          ++it )
    {
      if ( mActiveSubLayerVisibility.find( *it ).value() )
      {
        visibleLayers += *it;
        visibleStyles += *it2;
      }

      ++it2;
    }

    QString layers = visibleLayers.join( "," );
    QString styles = visibleStyles.join( "," );

    QgsDebugMsg( "Visible layer list of " + layers + " and style list of " + styles );

    QUrl url( mIgnoreGetMapUrl ? mBaseUrl : getMapUrl() );
    setQueryItem( url, "SERVICE", "WMS" );
    setQueryItem( url, "VERSION", mCapabilities.version );
    setQueryItem( url, "REQUEST", "GetMap" );
    setQueryItem( url, "BBOX", bbox );
    setQueryItem( url, crsKey, mImageCrs );
    setQueryItem( url, "WIDTH", QString::number( pixelWidth ) );
    setQueryItem( url, "HEIGHT", QString::number( pixelHeight ) );
    setQueryItem( url, "LAYERS", layers );
    setQueryItem( url, "STYLES", styles );
    setQueryItem( url, "FORMAT", mImageMimeType );

    if ( mDpi != -1 )
    {
      if ( mDpiMode & dpiQGIS )
        setQueryItem( url, "DPI", QString::number( mDpi ) );
      if ( mDpiMode & dpiUMN )
        setQueryItem( url, "MAP_RESOLUTION", QString::number( mDpi ) );
      if ( mDpiMode & dpiGeoServer )
        setQueryItem( url, "FORMAT_OPTIONS", QString( "dpi:%1" ).arg( mDpi ) );
    }

    //MH: jpeg does not support transparency and some servers complain if jpg and transparent=true
    if ( mImageMimeType == "image/x-jpegorpng" ||
         ( !mImageMimeType.contains( "jpeg", Qt::CaseInsensitive ) &&
           !mImageMimeType.contains( "jpg", Qt::CaseInsensitive ) ) )
    {
      setQueryItem( url, "TRANSPARENT", "TRUE" );  // some servers giving error for 'true' (lowercase)
    }

    QgsDebugMsg( QString( "getmap: %1" ).arg( url.toString() ) );

    // cache some details for if the user wants to do an identifyAsHtml() later

    mGetFeatureInfoUrlBase = mIgnoreGetFeatureInfoUrl ? mBaseUrl : getFeatureInfoUrl();

    QNetworkRequest request( url );
    setAuthorization( request );
    request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
    mCacheReply = QgsNetworkAccessManager::instance()->get( request );
    connect( mCacheReply, SIGNAL( finished() ), this, SLOT( cacheReplyFinished() ) );
    connect( mCacheReply, SIGNAL( downloadProgress( qint64, qint64 ) ), this, SLOT( cacheReplyProgress( qint64, qint64 ) ) );

    emit statusChanged( tr( "Getting map via WMS." ) );

    mWaiting = true;

    QTime t;
    t.start();

    while ( mCacheReply && ( !bkLayerCaching || t.elapsed() < WMS_THRESHOLD ) )
    {
      QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents, WMS_THRESHOLD );
    }

    mWaiting = false;
  }
  else
  {
    mTileReqNo++;

    double vres = viewExtent.width() / pixelWidth;
    double tres = vres;

    const QgsWmtsTileMatrix *tm = 0;
    enum QgsTileMode tileMode;

    if ( mTiled )
    {
      Q_ASSERT( mTileLayer );
      Q_ASSERT( mTileMatrixSet );
      Q_ASSERT( mTileMatrixSet->tileMatrices.size() > 0 );

      QMap<double, QgsWmtsTileMatrix> &m =  mTileMatrixSet->tileMatrices;

      // find nearest resolution
      QMap<double, QgsWmtsTileMatrix>::const_iterator prev, it = m.constBegin();
      while ( it != m.constEnd() && it.key() < vres )
      {
        QgsDebugMsg( QString( "res:%1 >= %2" ).arg( it.key() ).arg( vres ) );
        prev = it;
        ++it;
      }

      if ( it == m.constEnd() ||
           ( it != m.constBegin() && vres - prev.key() < it.key() - vres ) )
      {
        QgsDebugMsg( "back to previous res" );
        it = prev;
      }

      tres = it.key();
      tm = &it.value();

      tileMode = mTileLayer->tileMode;
    }
    else
    {
      static QgsWmtsTileMatrix tempTm;
      tempTm.topLeft      = QgsPoint( mLayerExtent.xMinimum(), mLayerExtent.yMaximum() );
      tempTm.tileWidth    = mMaxWidth;
      tempTm.tileHeight   = mMaxHeight;
      tempTm.matrixWidth  = ceil( mLayerExtent.width() / mMaxWidth / vres );
      tempTm.matrixHeight = ceil( mLayerExtent.height() / mMaxHeight / vres );
      tm = &tempTm;

      tileMode = WMSC;
    }

    QgsDebugMsg( QString( "layer extent: %1,%2 %3x%4" )
                 .arg( qgsDoubleToString( mLayerExtent.xMinimum() ) )
                 .arg( qgsDoubleToString( mLayerExtent.yMinimum() ) )
                 .arg( mLayerExtent.width() )
                 .arg( mLayerExtent.height() )
               );

    QgsDebugMsg( QString( "view extent: %1,%2 %3x%4  res:%5" )
                 .arg( qgsDoubleToString( viewExtent.xMinimum() ) )
                 .arg( qgsDoubleToString( viewExtent.yMinimum() ) )
                 .arg( viewExtent.width() )
                 .arg( viewExtent.height() )
                 .arg( vres, 0, 'f' )
               );

    QgsDebugMsg( QString( "tile matrix %1,%2 res:%3 tilesize:%4x%5 matrixsize:%6x%7 id:%8" )
                 .arg( tm->topLeft.x() ).arg( tm->topLeft.y() ).arg( tres )
                 .arg( tm->tileWidth ).arg( tm->tileHeight )
                 .arg( tm->matrixWidth ).arg( tm->matrixHeight )
                 .arg( tm->identifier )
               );

    // calculate tile coordinates
    double twMap = tm->tileWidth * tres;
    double thMap = tm->tileHeight * tres;
    QgsDebugMsg( QString( "tile map size: %1,%2" ).arg( qgsDoubleToString( twMap ) ).arg( qgsDoubleToString( thMap ) ) );

    int minTileCol = 0;
    int maxTileCol = tm->matrixWidth - 1;
    int minTileRow = 0;
    int maxTileRow = tm->matrixHeight - 1;


    if ( mTileLayer &&
         mTileLayer->setLinks.contains( mTileMatrixSet->identifier ) &&
         mTileLayer->setLinks[ mTileMatrixSet->identifier ].limits.contains( tm->identifier ) )
    {
      const QgsWmtsTileMatrixLimits &tml = mTileLayer->setLinks[ mTileMatrixSet->identifier ].limits[ tm->identifier ];
      minTileCol = tml.minTileCol;
      maxTileCol = tml.maxTileCol;
      minTileRow = tml.minTileRow;
      maxTileRow = tml.maxTileRow;
      QgsDebugMsg( QString( "%1 %2: TileMatrixLimits col %3-%4 row %5-%6" )
                   .arg( mTileMatrixSet->identifier )
                   .arg( tm->identifier )
                   .arg( minTileCol ).arg( maxTileCol )
                   .arg( minTileRow ).arg( maxTileRow ) );
    }

    int col0 = qBound( minTileCol, ( int ) floor(( viewExtent.xMinimum() - tm->topLeft.x() ) / twMap ), maxTileCol );
    int row0 = qBound( minTileRow, ( int ) floor(( tm->topLeft.y() - viewExtent.yMaximum() ) / thMap ), maxTileRow );
    int col1 = qBound( minTileCol, ( int ) floor(( viewExtent.xMaximum() - tm->topLeft.x() ) / twMap ), maxTileCol );
    int row1 = qBound( minTileRow, ( int ) floor(( tm->topLeft.y() - viewExtent.yMinimum() ) / thMap ), maxTileRow );

#if QGISDEBUG
    int n = ( col1 - col0 + 1 ) * ( row1 - row0 + 1 );
    QgsDebugMsg( QString( "tile number: %1x%2 = %3" ).arg( col1 - col0 + 1 ).arg( row1 - row0 + 1 ).arg( n ) );
    if ( n > 100 )
    {
      emit statusChanged( QString( "current view would need %1 tiles. tile request per draw limited to 100." ).arg( n ) );
      return mCachedImage;
    }
#endif

    switch ( tileMode )
    {
      case WMSC:
      {
        // add WMS request
        QUrl url( mIgnoreGetMapUrl ? mBaseUrl : getMapUrl() );
        setQueryItem( url, "SERVICE", "WMS" );
        setQueryItem( url, "VERSION", mCapabilities.version );
        setQueryItem( url, "REQUEST", "GetMap" );
        setQueryItem( url, "WIDTH", QString::number( tm->tileWidth ) );
        setQueryItem( url, "HEIGHT", QString::number( tm->tileHeight ) );
        setQueryItem( url, "LAYERS", mActiveSubLayers.join( "," ) );
        setQueryItem( url, "STYLES", mActiveSubStyles.join( "," ) );
        setQueryItem( url, "FORMAT", mImageMimeType );
        setQueryItem( url, crsKey, mImageCrs );

        if ( mTiled )
        {
          setQueryItem( url, "TILED", "true" );
        }

        if ( mDpi != -1 )
        {
          if ( mDpiMode & dpiQGIS )
            setQueryItem( url, "DPI", QString::number( mDpi ) );
          if ( mDpiMode & dpiUMN )
            setQueryItem( url, "MAP_RESOLUTION", QString::number( mDpi ) );
          if ( mDpiMode & dpiGeoServer )
            setQueryItem( url, "FORMAT_OPTIONS", QString( "dpi:%1" ).arg( mDpi ) );
        }

        if ( mImageMimeType == "image/x-jpegorpng" ||
             ( !mImageMimeType.contains( "jpeg", Qt::CaseInsensitive ) &&
               !mImageMimeType.contains( "jpg", Qt::CaseInsensitive ) ) )
        {
          setQueryItem( url, "TRANSPARENT", "TRUE" );  // some servers giving error for 'true' (lowercase)
        }

        int i = 0;
        for ( int row = row0; row <= row1; row++ )
        {
          for ( int col = col0; col <= col1; col++ )
          {
            QString turl;
            turl += url.toString();
            turl += QString( changeXY ? "&BBOX=%2,%1,%4,%3" : "&BBOX=%1,%2,%3,%4" )
                    .arg( qgsDoubleToString( tm->topLeft.x() +         col * twMap /* + twMap * 0.001 */ ) )
                    .arg( qgsDoubleToString( tm->topLeft.y() - ( row + 1 ) * thMap /* - thMap * 0.001 */ ) )
                    .arg( qgsDoubleToString( tm->topLeft.x() + ( col + 1 ) * twMap /* - twMap * 0.001 */ ) )
                    .arg( qgsDoubleToString( tm->topLeft.y() -         row * thMap /* + thMap * 0.001 */ ) );

            QNetworkRequest request( turl );
            setAuthorization( request );
            QgsDebugMsg( QString( "tileRequest %1 %2/%3 (%4,%5): %6" ).arg( mTileReqNo ).arg( i++ ).arg( n ).arg( row ).arg( col ).arg( turl ) );
            request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
            request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
            request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ), mTileReqNo );
            request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileIndex ), i );
            request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRect ),
                                  QRectF( tm->topLeft.x() + col * twMap, tm->topLeft.y() - ( row + 1 ) * thMap, twMap, thMap ) );
            request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRetry ), 0 );

            QgsDebugMsg( QString( "gettile: %1" ).arg( turl ) );
            QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
            mTileReplies << reply;
            connect( reply, SIGNAL( finished() ), this, SLOT( tileReplyFinished() ) );
          }
        }
      }
      break;

      case WMTS:
      {
        if ( !getTileUrl().isNull() )
        {
          // KVP
          QUrl url( mIgnoreGetMapUrl ? mBaseUrl : getTileUrl() );

          // compose static request arguments.
          setQueryItem( url, "SERVICE", "WMTS" );
          setQueryItem( url, "REQUEST", "GetTile" );
          setQueryItem( url, "VERSION", mCapabilities.version );
          setQueryItem( url, "LAYER", mActiveSubLayers[0] );
          setQueryItem( url, "STYLE", mActiveSubStyles[0] );
          setQueryItem( url, "FORMAT", mImageMimeType );
          setQueryItem( url, "TILEMATRIXSET", mTileMatrixSet->identifier );
          setQueryItem( url, "TILEMATRIX", tm->identifier );

          for ( QHash<QString, QString>::const_iterator it = mTileDimensionValues.constBegin(); it != mTileDimensionValues.constEnd(); ++it )
          {
            setQueryItem( url, it.key(), it.value() );
          }

          url.removeQueryItem( "TILEROW" );
          url.removeQueryItem( "TILECOL" );

          int i = 0;
          for ( int row = row0; row <= row1; row++ )
          {
            for ( int col = col0; col <= col1; col++ )
            {
              QString turl;
              turl += url.toString();
              turl += QString( "&TILEROW=%1&TILECOL=%2" ).arg( row ).arg( col );

              QNetworkRequest request( turl );
              setAuthorization( request );
              QgsDebugMsg( QString( "tileRequest %1 %2/%3 (%4,%5): %6" ).arg( mTileReqNo ).arg( i++ ).arg( n ).arg( row ).arg( col ).arg( turl ) );
              request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
              request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ), mTileReqNo );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileIndex ), i );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRect ),
                                    QRectF( tm->topLeft.x() + col * twMap, tm->topLeft.y() - ( row + 1 ) * thMap, twMap, thMap ) );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRetry ), 0 );

              QgsDebugMsg( QString( "gettile: %1" ).arg( turl ) );
              QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
              mTileReplies << reply;
              connect( reply, SIGNAL( finished() ), this, SLOT( tileReplyFinished() ) );
            }
          }
        }
        else
        {
          // REST
          QString url = mTileLayer->getTileURLs[ mImageMimeType ];

          url.replace( "{style}", mActiveSubStyles[0], Qt::CaseInsensitive );
          url.replace( "{tilematrixset}", mTileMatrixSet->identifier, Qt::CaseInsensitive );
          url.replace( "{tilematrix}", tm->identifier, Qt::CaseInsensitive );

          for ( QHash<QString, QString>::const_iterator it = mTileDimensionValues.constBegin(); it != mTileDimensionValues.constEnd(); ++it )
          {
            url.replace( "{" + it.key() + "}", it.value(), Qt::CaseInsensitive );
          }

          int i = 0;
          for ( int row = row0; row <= row1; row++ )
          {
            for ( int col = col0; col <= col1; col++ )
            {
              QString turl( url );
              turl.replace( "{tilerow}", QString::number( row ), Qt::CaseInsensitive );
              turl.replace( "{tilecol}", QString::number( col ), Qt::CaseInsensitive );

              QNetworkRequest request( turl );
              setAuthorization( request );
              QgsDebugMsg( QString( "tileRequest %1 %2/%3 (%4,%5): %6" ).arg( mTileReqNo ).arg( i++ ).arg( n ).arg( row ).arg( col ).arg( turl ) );
              request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
              request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ), mTileReqNo );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileIndex ), i );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRect ),
                                    QRectF( tm->topLeft.x() + col * twMap, tm->topLeft.y() - ( row + 1 ) * thMap, twMap, thMap ) );
              request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRetry ), 0 );

              QgsDebugMsg( QString( "gettile: %1" ).arg( turl ) );
              QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
              mTileReplies << reply;
              connect( reply, SIGNAL( finished() ), this, SLOT( tileReplyFinished() ) );
            }
          }
        }
      }
      break;

      default:
        QgsDebugMsg( QString( "unexpected tile mode %1" ).arg( mTileLayer->tileMode ) );
        return mCachedImage;
        break;
    }

    emit statusChanged( tr( "Getting tiles." ) );

    mWaiting = true;

    QTime t;
    t.start();

    // draw everything that is retrieved within a second
    // and the rest asynchronously
    while ( !mTileReplies.isEmpty() && ( !bkLayerCaching || t.elapsed() < WMS_THRESHOLD ) )
    {
      QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents, WMS_THRESHOLD );
    }

    mWaiting = false;

#ifdef QGISDEBUG
    emit statusChanged( tr( "%n tile requests in background", "tile request count", mTileReplies.count() )
                        + tr( ", %n cache hits", "tile cache hits", mCacheHits )
                        + tr( ", %n cache misses.", "tile cache missed", mCacheMisses )
                        + tr( ", %n errors.", "errors", mErrors )
                      );
#endif
  }

  return mCachedImage;
}

void QgsWmsProvider::readBlock( int bandNo, QgsRectangle  const & viewExtent, int pixelWidth, int pixelHeight, void *block )
{
  Q_UNUSED( bandNo );
  QgsDebugMsg( "Entered" );
  // TODO: optimize to avoid writing to QImage
  QImage *image = draw( viewExtent, pixelWidth, pixelHeight );
  if ( !image )   // should not happen
  {
    QgsMessageLog::logMessage( tr( "image is NULL" ), tr( "WMS" ) );
    return;
  }

  QgsDebugMsg( QString( "image height = %1 bytesPerLine = %2" ).arg( image->height() ) . arg( image->bytesPerLine() ) ) ;
  size_t myExpectedSize = pixelWidth * pixelHeight * 4;
  size_t myImageSize = image->height() *  image->bytesPerLine();
  if ( myExpectedSize != myImageSize )   // should not happen
  {
    QgsMessageLog::logMessage( tr( "unexpected image size" ), tr( "WMS" ) );
    return;
  }

  uchar * ptr = image->bits() ;
  if ( ptr )
  {
    // If image is too large, ptr can be NULL
    memcpy( block, ptr, myExpectedSize );
  }
  // do not delete the image, it is handled by draw()
  //delete image;
}

void QgsWmsProvider::repeatTileRequest( QNetworkRequest const &oldRequest )
{
  if ( mErrors == 100 )
  {
    QgsMessageLog::logMessage( tr( "Not logging more than 100 request errors." ), tr( "WMS" ) );
  }

  QNetworkRequest request( oldRequest );

  QString url = request.url().toString();
  int tileReqNo = request.attribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ) ).toInt();
  int tileNo = request.attribute( static_cast<QNetworkRequest::Attribute>( TileIndex ) ).toInt();
  int retry = request.attribute( static_cast<QNetworkRequest::Attribute>( TileRetry ) ).toInt();
  retry++;

  QSettings s;
  int maxRetry = s.value( "/qgis/defaultTileMaxRetry", "3" ).toInt();
  if ( retry > maxRetry )
  {
    if ( mErrors < 100 )
    {
      QgsMessageLog::logMessage( tr( "Tile request max retry error. Failed %1 requests for tile %2 of tileRequest %3 (url: %4)" )
                                 .arg( maxRetry ).arg( tileNo ).arg( tileReqNo ).arg( url ), tr( "WMS" ) );
    }
    return;
  }

  setAuthorization( request );
  if ( mErrors < 100 )
  {
    QgsMessageLog::logMessage( tr( "repeat tileRequest %1 tile %2(retry %3)" )
                               .arg( tileReqNo ).arg( tileNo ).arg( retry ), tr( "WMS" ), QgsMessageLog::INFO );
  }
  QgsDebugMsg( QString( "repeat tileRequest %1 %2(retry %3) for url: %4" ).arg( tileReqNo ).arg( tileNo ).arg( retry ).arg( url ) );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRetry ), retry );

  QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
  mTileReplies << reply;
  connect( reply, SIGNAL( finished() ), this, SLOT( tileReplyFinished() ) );
}

void QgsWmsProvider::tileReplyFinished()
{
  QNetworkReply *reply = qobject_cast<QNetworkReply*>( sender() );

#if defined(QGISDEBUG)
  bool fromCache = reply->attribute( QNetworkRequest::SourceIsFromCacheAttribute ).toBool();
  if ( fromCache )
    mCacheHits++;
  else
    mCacheMisses++;
#endif
#if defined(QGISDEBUG)
  QgsDebugMsgLevel( "raw headers:", 3 );
  foreach ( const QNetworkReply::RawHeaderPair &pair, reply->rawHeaderPairs() )
  {
    QgsDebugMsgLevel( QString( " %1:%2" )
                      .arg( QString::fromUtf8( pair.first ) )
                      .arg( QString::fromUtf8( pair.second ) ), 3 );
  }
#endif

  if ( QgsNetworkAccessManager::instance()->cache() )
  {
    QNetworkCacheMetaData cmd = QgsNetworkAccessManager::instance()->cache()->metaData( reply->request().url() );

    QNetworkCacheMetaData::RawHeaderList hl;
    foreach ( const QNetworkCacheMetaData::RawHeader &h, cmd.rawHeaders() )
    {
      if ( h.first != "Cache-Control" )
        hl.append( h );
    }
    cmd.setRawHeaders( hl );

    QgsDebugMsg( QString( "expirationDate:%1" ).arg( cmd.expirationDate().toString() ) );
    if ( cmd.expirationDate().isNull() )
    {
      QSettings s;
      cmd.setExpirationDate( QDateTime::currentDateTime().addSecs( s.value( "/qgis/defaultTileExpiry", "24" ).toInt() * 60 * 60 ) );
    }

    QgsNetworkAccessManager::instance()->cache()->updateMetaData( cmd );
  }

  int tileReqNo = reply->request().attribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ) ).toInt();
  int tileNo = reply->request().attribute( static_cast<QNetworkRequest::Attribute>( TileIndex ) ).toInt();
  QRectF r = reply->request().attribute( static_cast<QNetworkRequest::Attribute>( TileRect ) ).toRectF();

#ifdef QGISDEBUG
  int retry = reply->request().attribute( static_cast<QNetworkRequest::Attribute>( TileRetry ) ).toInt();

  QgsDebugMsg( QString( "tile reply %1 (%2) tile:%3(retry %4) rect:%5,%6 %7,%8) fromcache:%9 error:%10 url:%11" )
               .arg( tileReqNo ).arg( mTileReqNo ).arg( tileNo ).arg( retry )
               .arg( r.left(), 0, 'f' ).arg( r.bottom(), 0, 'f' ).arg( r.right(), 0, 'f' ).arg( r.top(), 0, 'f' )
               .arg( fromCache )
               .arg( reply->errorString() )
               .arg( reply->url().toString() )
             );
#endif

  if ( reply->error() == QNetworkReply::NoError )
  {
    QVariant redirect = reply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( !redirect.isNull() )
    {
      QNetworkRequest request( redirect.toUrl() );
      setAuthorization( request );
      request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache );
      request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );
      request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileReqNo ), tileReqNo );
      request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileIndex ), tileNo );
      request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRect ), r );
      request.setAttribute( static_cast<QNetworkRequest::Attribute>( TileRetry ), 0 );

      mTileReplies.removeOne( reply );
      reply->deleteLater();

      QgsDebugMsg( QString( "redirected gettile: %1" ).arg( redirect.toString() ) );
      reply = QgsNetworkAccessManager::instance()->get( request );
      mTileReplies << reply;

      connect( reply, SIGNAL( finished() ), this, SLOT( tileReplyFinished() ) );

      return;
    }

    QVariant status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    if ( !status.isNull() && status.toInt() >= 400 )
    {
      QVariant phrase = reply->attribute( QNetworkRequest::HttpReasonPhraseAttribute );

      showMessageBox( tr( "Tile request error" ), tr( "Status: %1\nReason phrase: %2" ).arg( status.toInt() ).arg( phrase.toString() ) );

      mTileReplies.removeOne( reply );
      reply->deleteLater();

      return;
    }

    QString contentType = reply->header( QNetworkRequest::ContentTypeHeader ).toString();
    QgsDebugMsg( "contentType: " + contentType );
    if ( !contentType.startsWith( "image/", Qt::CaseInsensitive ) &&
         contentType.compare( "application/octet-stream", Qt::CaseInsensitive ) != 0 )
    {
      QByteArray text = reply->readAll();
      if ( contentType.toLower() == "text/xml" && parseServiceExceptionReportDom( text ) )
      {
        QgsMessageLog::logMessage( tr( "Tile request error (Title:%1; Error:%2; URL: %3)" )
                                   .arg( mErrorCaption ).arg( mError )
                                   .arg( reply->url().toString() ), tr( "WMS" ) );
      }
      else
      {
        QgsMessageLog::logMessage( tr( "Tile request error (Status:%1; Content-Type:%2; Length:%3; URL: %4)" )
                                   .arg( status.toString() )
                                   .arg( contentType )
                                   .arg( text.size() )
                                   .arg( reply->url().toString() ), tr( "WMS" ) );
#ifdef QGISDEBUG
        QFile file( QDir::tempPath() + "/broken-image.png" );
        if ( file.open( QIODevice::WriteOnly ) )
        {
          file.write( text );
          file.close();
        }
#endif
      }

      mTileReplies.removeOne( reply );
      reply->deleteLater();

      return;
    }

    // only take results from current request number
    if ( mTileReqNo == tileReqNo )
    {
      double cr = mCachedViewExtent.width() / mCachedViewWidth;

      QRectF dst(( r.left() - mCachedViewExtent.xMinimum() ) / cr,
                 ( mCachedViewExtent.yMaximum() - r.bottom() ) / cr,
                 r.width() / cr,
                 r.height() / cr );

      QgsDebugMsg( QString( "tile reply: length %1" ).arg( reply->bytesAvailable() ) );

      QImage myLocalImage = QImage::fromData( reply->readAll() );

      if ( !myLocalImage.isNull() )
      {
        QPainter p( mCachedImage );
        if ( mSmoothPixmapTransform )
          p.setRenderHint( QPainter::SmoothPixmapTransform, true );
        p.drawImage( dst, myLocalImage );
#if 0
        myLocalImage.save( QString( "%1/%2-tile-%3.png" ).arg( QDir::tempPath() ).arg( mTileReqNo ).arg( tileNo ) );
        p.drawRect( dst ); // show tile bounds
        p.drawText( dst, Qt::AlignCenter, QString( "(%1)\n%2,%3\n%4,%5\n%6x%7" )
                    .arg( tileNo )
                    .arg( r.left() ).arg( r.bottom() )
                    .arg( r.right() ).arg( r.top() )
                    .arg( r.width() ).arg( r.height() ) );
#endif
      }
      else
      {
        QgsMessageLog::logMessage( tr( "Returned image is flawed [Content-Type:%1; URL: %2]" )
                                   .arg( contentType ).arg( reply->url().toString() ), tr( "WMS" ) );

        repeatTileRequest( reply->request() );
      }
    }
    else
    {
      QgsDebugMsg( QString( "Reply too late [%1]" ).arg( reply->url().toString() ) );
    }

    mTileReplies.removeOne( reply );
    reply->deleteLater();

    if ( !mWaiting )
    {
      QgsDebugMsg( "emit dataChanged()" );
      emit dataChanged();
    }
  }
  else
  {
    mErrors++;

    repeatTileRequest( reply->request() );

    mTileReplies.removeOne( reply );
    reply->deleteLater();
  }

#ifdef QGISDEBUG
  emit statusChanged( tr( "%n tile requests in background", "tile request count", mTileReplies.count() )
                      + tr( ", %n cache hits", "tile cache hits", mCacheHits )
                      + tr( ", %n cache misses.", "tile cache missed", mCacheMisses )
                      + tr( ", %n errors.", "errors", mErrors )
                    );
#endif
}

void QgsWmsProvider::cacheReplyFinished()
{
  if ( mCacheReply->error() == QNetworkReply::NoError )
  {
    QVariant redirect = mCacheReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( !redirect.isNull() )
    {
      mCacheReply->deleteLater();

      QgsDebugMsg( QString( "redirected getmap: %1" ).arg( redirect.toString() ) );
      mCacheReply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( redirect.toUrl() ) );
      connect( mCacheReply, SIGNAL( finished() ), this, SLOT( cacheReplyFinished() ) );
      return;
    }

    QVariant status = mCacheReply->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    if ( !status.isNull() && status.toInt() >= 400 )
    {
      QVariant phrase = mCacheReply->attribute( QNetworkRequest::HttpReasonPhraseAttribute );

      QgsMessageLog::logMessage( tr( "Map request error (Status: %1; Reason phrase: %2; URL:%3)" )
                                 .arg( status.toInt() )
                                 .arg( phrase.toString() )
                                 .arg( mCacheReply->url().toString() ), tr( "WMS" ) );

      mCacheReply->deleteLater();
      mCacheReply = 0;

      return;
    }

    QString contentType = mCacheReply->header( QNetworkRequest::ContentTypeHeader ).toString();
    QgsDebugMsg( "contentType: " + contentType );
    QByteArray text = mCacheReply->readAll();
    QImage myLocalImage = QImage::fromData( text );

    if ( !myLocalImage.isNull() )
    {
      QPainter p( mCachedImage );
      p.drawImage( 0, 0, myLocalImage );
    }
    else if ( contentType.startsWith( "image/", Qt::CaseInsensitive ) ||
              contentType.compare( "application/octet-stream", Qt::CaseInsensitive ) == 0 )
    {
      QgsMessageLog::logMessage( tr( "Returned image is flawed [Content-Type:%1; URL:%2]" )
                                 .arg( contentType ).arg( mCacheReply->url().toString() ), tr( "WMS" ) );
    }
    else
    {
      if ( contentType.toLower() == "text/xml" && parseServiceExceptionReportDom( text ) )
      {
        QgsMessageLog::logMessage( tr( "Map request error (Title:%1; Error:%2; URL: %3)" )
                                   .arg( mErrorCaption ).arg( mError )
                                   .arg( mCacheReply->url().toString() ), tr( "WMS" ) );
      }
      else
      {
        QgsMessageLog::logMessage( tr( "Map request error (Status: %1; Response: %2; Content-Type: %3; URL:%4)" )
                                   .arg( status.toInt() )
                                   .arg( QString::fromUtf8( text ) )
                                   .arg( contentType )
                                   .arg( mCacheReply->url().toString() ), tr( "WMS" ) );
      }
    }

    mCacheReply->deleteLater();
    mCacheReply = 0;

    if ( !mWaiting && !myLocalImage.isNull() )
    {
      QgsDebugMsg( "emit dataChanged()" );
      emit dataChanged();
    }
  }
  else
  {
    mErrors++;
    if ( mErrors < 100 )
    {
      QgsMessageLog::logMessage( tr( "Map request failed [error:%1 url:%2]" ).arg( mCacheReply->errorString() ).arg( mCacheReply->url().toString() ), tr( "WMS" ) );
    }
    else if ( mErrors == 100 )
    {
      QgsMessageLog::logMessage( tr( "Not logging more than 100 request errors." ), tr( "WMS" ) );
    }

    mCacheReply->deleteLater();
    mCacheReply = 0;
  }
}

bool QgsWmsProvider::retrieveServerCapabilities( bool forceRefresh )
{
  QgsDebugMsg( "entering." );

  if ( mHttpCapabilitiesResponse.isNull() || forceRefresh )
  {
    QString url = mBaseUrl;
    QgsDebugMsg( "url = " + url );
    if ( !url.contains( "SERVICE=WMTS" ) &&
         !url.contains( "/WMTSCapabilities.xml" ) )
    {
      url += "SERVICE=WMS&REQUEST=GetCapabilities";
    }

    mError = "";

    QNetworkRequest request( url );
    setAuthorization( request );
    request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork );
    request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );

    QgsDebugMsg( QString( "getcapabilities: %1" ).arg( url ) );
    mCapabilitiesReply = QgsNetworkAccessManager::instance()->get( request );

    connect( mCapabilitiesReply, SIGNAL( finished() ), this, SLOT( capabilitiesReplyFinished() ) );
    connect( mCapabilitiesReply, SIGNAL( downloadProgress( qint64, qint64 ) ), this, SLOT( capabilitiesReplyProgress( qint64, qint64 ) ) );

    while ( mCapabilitiesReply )
    {
      QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents );
    }

    if ( mHttpCapabilitiesResponse.isEmpty() )
    {
      if ( mError.isEmpty() )
      {
        mErrorFormat = "text/plain";
        mError = tr( "empty capabilities document" );
      }
      QgsDebugMsg( "response is empty" );
      return false;
    }

    if ( mHttpCapabilitiesResponse.startsWith( "<html>" ) ||
         mHttpCapabilitiesResponse.startsWith( "<HTML>" ) )
    {
      mErrorFormat = "text/html";
      mError = mHttpCapabilitiesResponse;
      QgsDebugMsg( "starts with <html>" );
      return false;
    }

    QgsDebugMsg( "Converting to Dom." );

    bool domOK;
    domOK = parseCapabilitiesDom( mHttpCapabilitiesResponse, mCapabilities );

    if ( !domOK )
    {
      // We had an Dom exception -
      // mErrorCaption and mError are pre-filled by parseCapabilitiesDom

      mError += tr( "\nTried URL: %1" ).arg( url );

      QgsDebugMsg( "!domOK: " + mError );

      return false;
    }
    else
    {
      // get identify formats
      foreach ( QString f, mCapabilities.capability.request.getFeatureInfo.format )
      {
        // Don't use mSupportedGetFeatureFormats, there are too many possibilities
#if 0
        if ( mSupportedGetFeatureFormats.contains( f ) )
        {
#endif
          QgsDebugMsg( "supported format = " + f );
          // 1.0: MIME - server shall choose format, we presume it to be plain text
          //      GML.1, GML.2, or GML.3
          // 1.1.0, 1.3.0 - mime types, GML should use application/vnd.ogc.gml
          //      but in UMN Mapserver it may be also OUTPUTFORMAT, e.g. OGRGML
          QgsRaster::IdentifyFormat format = QgsRaster::IdentifyFormatUndefined;
          if ( f == "MIME" )
            format = QgsRaster::IdentifyFormatText; // 1.0
          else if ( f == "text/plain" )
            format = QgsRaster::IdentifyFormatText;
          else if ( f == "text/html" )
            format = QgsRaster::IdentifyFormatHtml;
          else if ( f.startsWith( "GML." ) )
            format = QgsRaster::IdentifyFormatFeature; // 1.0
          else if ( f == "application/vnd.ogc.gml" )
            format = QgsRaster::IdentifyFormatFeature;
          else if ( f.contains( "gml", Qt::CaseInsensitive ) )
            format = QgsRaster::IdentifyFormatFeature;

          mIdentifyFormats.insert( format, f );
#if 0
        }
#endif
      }
    }
  }

  QgsDebugMsg( "exiting." );

  return mError.isEmpty();
}

void QgsWmsProvider::capabilitiesReplyFinished()
{
  QgsDebugMsg( "entering." );
  if ( mCapabilitiesReply->error() == QNetworkReply::NoError )
  {
    QgsDebugMsg( "reply ok" );
    QVariant redirect = mCapabilitiesReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( !redirect.isNull() )
    {
      emit statusChanged( tr( "Capabilities request redirected." ) );

      const QUrl& toUrl = redirect.toUrl();
      mCapabilitiesReply->request();
      if ( toUrl == mCapabilitiesReply->url() )
      {
        mErrorFormat = "text/plain";
        mError = tr( "Redirect loop detected: %1" ).arg( toUrl.toString() );
        QgsMessageLog::logMessage( mError, tr( "WMS" ) );
        mHttpCapabilitiesResponse.clear();
      }
      else
      {
        QNetworkRequest request( toUrl );
        setAuthorization( request );
        request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork );
        request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );

        mCapabilitiesReply->deleteLater();
        QgsDebugMsg( QString( "redirected getcapabilities: %1" ).arg( redirect.toString() ) );
        mCapabilitiesReply = QgsNetworkAccessManager::instance()->get( request );

        connect( mCapabilitiesReply, SIGNAL( finished() ), this, SLOT( capabilitiesReplyFinished() ) );
        connect( mCapabilitiesReply, SIGNAL( downloadProgress( qint64, qint64 ) ), this, SLOT( capabilitiesReplyProgress( qint64, qint64 ) ) );
        return;
      }
    }
    else
    {
      mHttpCapabilitiesResponse = mCapabilitiesReply->readAll();

      if ( mHttpCapabilitiesResponse.isEmpty() )
      {
        mErrorFormat = "text/plain";
        mError = tr( "empty of capabilities: %1" ).arg( mCapabilitiesReply->errorString() );
      }
    }
  }
  else
  {
    mErrorFormat = "text/plain";
    mError = tr( "Download of capabilities failed: %1" ).arg( mCapabilitiesReply->errorString() );
    QgsMessageLog::logMessage( mError, tr( "WMS" ) );
    mHttpCapabilitiesResponse.clear();
  }

  mCapabilitiesReply->deleteLater();
  mCapabilitiesReply = 0;
}

QGis::DataType QgsWmsProvider::dataType( int bandNo ) const
{
  return srcDataType( bandNo );
}

QGis::DataType QgsWmsProvider::srcDataType( int bandNo ) const
{
  Q_UNUSED( bandNo );
  return QGis::ARGB32;
}

int QgsWmsProvider::bandCount() const
{
  return 1;
}

void QgsWmsProvider::capabilitiesReplyProgress( qint64 bytesReceived, qint64 bytesTotal )
{
  QString msg = tr( "%1 of %2 bytes of capabilities downloaded." ).arg( bytesReceived ).arg( bytesTotal < 0 ? QString( "unknown number of" ) : QString::number( bytesTotal ) );
  QgsDebugMsg( msg );
  emit statusChanged( msg );
}

void QgsWmsProvider::cacheReplyProgress( qint64 bytesReceived, qint64 bytesTotal )
{
  QString msg = tr( "%1 of %2 bytes of map downloaded." ).arg( bytesReceived ).arg( bytesTotal < 0 ? QString( "unknown number of" ) : QString::number( bytesTotal ) );
  QgsDebugMsg( msg );
  emit statusChanged( msg );
}

bool QgsWmsProvider::parseCapabilitiesDom( QByteArray const &xml, QgsWmsCapabilitiesProperty& capabilitiesProperty )
{
  QgsDebugMsg( "entering." );

#ifdef QGISDEBUG
  QFile file( QDir::tempPath() + "/qgis-wmsprovider-capabilities.xml" );
  if ( file.open( QIODevice::WriteOnly ) )
  {
    file.write( xml );
    file.close();
  }
#endif

  // Convert completed document into a Dom
  QString errorMsg;
  int errorLine;
  int errorColumn;
  bool contentSuccess = mCapabilitiesDom.setContent( xml, false, &errorMsg, &errorLine, &errorColumn );

  if ( !contentSuccess )
  {
    mErrorCaption = tr( "Dom Exception" );
    mErrorFormat = "text/plain";
    mError = tr( "Could not get WMS capabilities: %1 at line %2 column %3\nThis is probably due to an incorrect WMS Server URL.\nResponse was:\n\n%4" )
             .arg( errorMsg )
             .arg( errorLine )
             .arg( errorColumn )
             .arg( QString( xml ) );

    QgsLogger::debug( "Dom Exception: " + mError );

    return false;
  }

  QDomElement docElem = mCapabilitiesDom.documentElement();

  // Assert that the DTD is what we expected (i.e. a WMS Capabilities document)
  QgsDebugMsg( "testing tagName " + docElem.tagName() );

  if (
    docElem.tagName() != "WMS_Capabilities"  &&   // (1.3 vintage)
    docElem.tagName() != "WMT_MS_Capabilities" && // (1.1.1 vintage)
    docElem.tagName() != "Capabilities"           // WMTS
  )
  {
    mErrorCaption = tr( "Dom Exception" );
    mErrorFormat = "text/plain";
    mError = tr( "Could not get WMS capabilities in the expected format (DTD): no %1 or %2 found.\nThis might be due to an incorrect WMS Server URL.\nTag:%3\nResponse was:\n%4" )
             .arg( "WMS_Capabilities" )
             .arg( "WMT_MS_Capabilities" )
             .arg( docElem.tagName() )
             .arg( QString( xml ) );

    QgsLogger::debug( "Dom Exception: " + mError );

    return false;
  }

  capabilitiesProperty.version = docElem.attribute( "version" );

  // Start walking through XML.
  QDomNode n = docElem.firstChild();

  while ( !n.isNull() )
  {
    QDomElement e = n.toElement(); // try to convert the node to an element.
    if ( !e.isNull() )
    {
      QgsDebugMsg( e.tagName() ); // the node really is an element.

      if ( e.tagName() == "Service" || e.tagName() == "ows:ServiceProvider" || e.tagName() == "ows:ServiceIdentification" )
      {
        QgsDebugMsg( "  Service." );
        parseService( e, capabilitiesProperty.service );
      }
      else if ( e.tagName() == "Capability" || e.tagName() == "ows:OperationsMetadata" )
      {
        QgsDebugMsg( "  Capability." );
        parseCapability( e, capabilitiesProperty.capability );
      }
      else if ( e.tagName() == "Contents" )
      {
        QgsDebugMsg( "  Contents." );
        parseWMTSContents( e );
      }
    }
    n = n.nextSibling();
  }

  QgsDebugMsg( "exiting." );

  return true;
}


void QgsWmsProvider::parseService( QDomElement const & e, QgsWmsServiceProperty& serviceProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      // QgsDebugMsg( "  "  + e1.tagName() ); // the node really is an element.
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );
      if ( tagName.startsWith( "ows:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Title" )
      {
        serviceProperty.title = e1.text();
      }
      else if ( tagName == "Abstract" )
      {
        serviceProperty.abstract = e1.text();
      }
      else if ( tagName == "KeywordList" || tagName == "Keywords" )
      {
        parseKeywordList( e1, serviceProperty.keywordList );
      }
      else if ( tagName == "OnlineResource" )
      {
        parseOnlineResource( e1, serviceProperty.onlineResource );
      }
      else if ( tagName == "ContactInformation" || tagName == "ServiceContact" )
      {
        parseContactInformation( e1, serviceProperty.contactInformation );
      }
      else if ( tagName == "Fees" )
      {
        serviceProperty.fees = e1.text();
      }
      else if ( tagName == "AccessConstraints" )
      {
        serviceProperty.accessConstraints = e1.text();
      }
      else if ( tagName == "LayerLimit" )
      {
        serviceProperty.layerLimit = e1.text().toUInt();
      }
      else if ( tagName == "MaxWidth" )
      {
        serviceProperty.maxWidth = e1.text().toUInt();
      }
      else if ( tagName == "MaxHeight" )
      {
        serviceProperty.maxHeight = e1.text().toUInt();
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseCapability( QDomElement const & e, QgsWmsCapabilityProperty& capabilityProperty )
{
  QgsDebugMsg( "entering." );

  for ( QDomNode n1 = e.firstChild(); !n1.isNull(); n1 = n1.nextSibling() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( e1.isNull() )
      continue;

    QString tagName = e1.tagName();
    if ( tagName.startsWith( "wms:" ) )
      tagName = tagName.mid( 4 );

    QgsDebugMsg( "  "  + e1.tagName() ); // the node really is an element.

    if ( tagName == "Request" )
    {
      parseRequest( e1, capabilityProperty.request );
    }
    else if ( tagName == "Layer" )
    {
      parseLayer( e1, capabilityProperty.layer );
    }
    else if ( tagName == "VendorSpecificCapabilities" )
    {
      for ( int i = 0; i < e1.childNodes().size(); i++ )
      {
        QDomNode n2 = e1.childNodes().item( i );
        QDomElement e2 = n2.toElement();

        QString tagName = e2.tagName();
        if ( tagName.startsWith( "wms:" ) )
          tagName = tagName.mid( 4 );

        if ( tagName == "TileSet" )
        {
          parseTileSetProfile( e2 );
        }
      }
    }
    else if ( tagName == "ows:Operation" )
    {
      QString name = e1.attribute( "name" );
      QDomElement get = n1.firstChildElement( "ows:DCP" )
                        .firstChildElement( "ows:HTTP" )
                        .firstChildElement( "ows:Get" );

      QString href = get.attribute( "xlink:href" );

      QgsWmsDcpTypeProperty dcp;
      dcp.http.get.onlineResource.xlinkHref = href;

      QgsWmsOperationType *ot = 0;
      if ( href.isNull() )
      {
        QgsDebugMsg( QString( "http get missing from ows:Operation '%1'" ).arg( name ) );
      }
      else if ( name == "GetTile" )
      {
        ot = &capabilityProperty.request.getTile;
      }
      else if ( name == "GetFeatureInfo" )
      {
        ot = &capabilityProperty.request.getFeatureInfo;
      }
      else if ( name == "GetLegendGraphic" || name == "sld:GetLegendGraphic" )
      {
        ot = &capabilityProperty.request.getLegendGraphic;
      }
      else
      {
        QgsDebugMsg( QString( "ows:Operation %1 ignored" ).arg( name ) );
      }

      if ( ot )
      {
        ot->dcpType << dcp;
        ot->allowedEncodings.clear();
        for ( QDomElement e2 = get.firstChildElement( "ows:Constraint" ).firstChildElement( "ows:AllowedValues" ).firstChildElement( "ows:Value" );
              !e2.isNull();
              e2 = e1.nextSiblingElement( "ows:Value" ) )
        {
          ot->allowedEncodings << e2.text();
        }
      }
    }
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseContactPersonPrimary( QDomElement const & e, QgsWmsContactPersonPrimaryProperty& contactPersonPrimaryProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "ContactPerson" )
      {
        contactPersonPrimaryProperty.contactPerson = e1.text();
      }
      else if ( tagName == "ContactOrganization" )
      {
        contactPersonPrimaryProperty.contactOrganization = e1.text();
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseContactAddress( QDomElement const & e, QgsWmsContactAddressProperty& contactAddressProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "AddressType" )
      {
        contactAddressProperty.addressType = e1.text();
      }
      else if ( tagName == "Address" )
      {
        contactAddressProperty.address = e1.text();
      }
      else if ( tagName == "City" )
      {
        contactAddressProperty.city = e1.text();
      }
      else if ( tagName == "StateOrProvince" )
      {
        contactAddressProperty.stateOrProvince = e1.text();
      }
      else if ( tagName == "PostCode" )
      {
        contactAddressProperty.postCode = e1.text();
      }
      else if ( tagName == "Country" )
      {
        contactAddressProperty.country = e1.text();
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseContactInformation( QDomElement const & e, QgsWmsContactInformationProperty& contactInformationProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "ContactPersonPrimary" )
      {
        parseContactPersonPrimary( e1, contactInformationProperty.contactPersonPrimary );
      }
      else if ( tagName == "ContactPosition" || tagName == "ows:PositionName" )
      {
        contactInformationProperty.contactPosition = e1.text();
      }
      else if ( tagName == "ContactAddress" )
      {
        parseContactAddress( e1, contactInformationProperty.contactAddress );
      }
      else if ( tagName == "ContactVoiceTelephone" )
      {
        contactInformationProperty.contactVoiceTelephone = e1.text();
      }
      else if ( tagName == "ContactFacsimileTelephone" )
      {
        contactInformationProperty.contactFacsimileTelephone = e1.text();
      }
      else if ( tagName == "ContactElectronicMailAddress" )
      {
        contactInformationProperty.contactElectronicMailAddress = e1.text();
      }
      else if ( tagName == "ows:IndividualName" )
      {
        contactInformationProperty.contactPersonPrimary.contactPerson = e1.text();
      }
      else if ( tagName == "ows:ProviderName" )
      {
        contactInformationProperty.contactPersonPrimary.contactOrganization = e1.text();
      }
      else if ( tagName == "ows:ContactInfo" )
      {
        QDomNode n = n1.firstChildElement( "ows:Phone" );
        contactInformationProperty.contactVoiceTelephone        = n.firstChildElement( "ows:Voice" ).toElement().text();
        contactInformationProperty.contactFacsimileTelephone    = n.firstChildElement( "ows:Facsimile" ).toElement().text();

        n = n1.firstChildElement( "ows:Address" );
        contactInformationProperty.contactElectronicMailAddress   = n.firstChildElement( "ows:ElectronicMailAddress" ).toElement().text();
        contactInformationProperty.contactAddress.address         = n.firstChildElement( "ows:DeliveryPoint" ).toElement().text();
        contactInformationProperty.contactAddress.city            = n.firstChildElement( "ows:City" ).toElement().text();
        contactInformationProperty.contactAddress.stateOrProvince = n.firstChildElement( "ows:AdministrativeArea" ).toElement().text();
        contactInformationProperty.contactAddress.postCode        = n.firstChildElement( "ows:PostalCode" ).toElement().text();
        contactInformationProperty.contactAddress.country         = n.firstChildElement( "ows:Country" ).toElement().text();
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseOnlineResource( QDomElement const & e, QgsWmsOnlineResourceAttribute& onlineResourceAttribute )
{
  QgsDebugMsg( "entering." );

  onlineResourceAttribute.xlinkHref = e.attribute( "xlink:href" );

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseKeywordList( QDomElement  const & e, QStringList& keywordListProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );
      if ( tagName.startsWith( "ows:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Keyword" )
      {
        QgsDebugMsg( "      Keyword." );
        keywordListProperty += e1.text();
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseGet( QDomElement const & e, QgsWmsGetProperty& getProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "OnlineResource" )
      {
        QgsDebugMsg( "      OnlineResource." );
        parseOnlineResource( e1, getProperty.onlineResource );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parsePost( QDomElement const & e, QgsWmsPostProperty& postProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "OnlineResource" )
      {
        QgsDebugMsg( "      OnlineResource." );
        parseOnlineResource( e1, postProperty.onlineResource );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseHttp( QDomElement const & e, QgsWmsHttpProperty& httpProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Get" )
      {
        QgsDebugMsg( "      Get." );
        parseGet( e1, httpProperty.get );
      }
      else if ( tagName == "Post" )
      {
        QgsDebugMsg( "      Post." );
        parsePost( e1, httpProperty.post );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseDcpType( QDomElement const & e, QgsWmsDcpTypeProperty& dcpType )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      if ( e1.tagName() == "HTTP" )
      {
        QgsDebugMsg( "      HTTP." );
        parseHttp( e1, dcpType.http );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseOperationType( QDomElement const & e, QgsWmsOperationType& operationType )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Format" )
      {
        QgsDebugMsg( "      Format." );
        operationType.format += e1.text();
      }
      else if ( tagName == "DCPType" )
      {
        QgsDebugMsg( "      DCPType." );
        QgsWmsDcpTypeProperty dcp;
        parseDcpType( e1, dcp );
        operationType.dcpType.push_back( dcp );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseRequest( QDomElement const & e, QgsWmsRequestProperty& requestProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString operation = e1.tagName();
      if ( operation == "Operation" )
      {
        operation = e1.attribute( "name" );
      }

      if ( operation == "GetMap" )
      {
        QgsDebugMsg( "      GetMap." );
        parseOperationType( e1, requestProperty.getMap );
      }
      else if ( operation == "GetFeatureInfo" )
      {
        QgsDebugMsg( "      GetFeatureInfo." );
        parseOperationType( e1, requestProperty.getFeatureInfo );
      }
      else if ( operation == "GetLegendGraphic" || operation == "sld:GetLegendGraphic" )
      {
        QgsDebugMsg( "      GetLegendGraphic." );
        parseOperationType( e1, requestProperty.getLegendGraphic );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseLegendUrl( QDomElement const & e, QgsWmsLegendUrlProperty& legendUrlProperty )
{
  QgsDebugMsg( "entering." );

  legendUrlProperty.width  = e.attribute( "width" ).toUInt();
  legendUrlProperty.height = e.attribute( "height" ).toUInt();

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Format" )
      {
        legendUrlProperty.format = e1.text();
      }
      else if ( tagName == "OnlineResource" )
      {
        parseOnlineResource( e1, legendUrlProperty.onlineResource );
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseStyle( QDomElement const & e, QgsWmsStyleProperty& styleProperty )
{
  QgsDebugMsg( "entering." );

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Name" )
      {
        styleProperty.name = e1.text();
      }
      else if ( tagName == "Title" )
      {
        styleProperty.title = e1.text();
      }
      else if ( tagName == "Abstract" )
      {
        styleProperty.abstract = e1.text();
      }
      else if ( tagName == "LegendURL" )
      {
        styleProperty.legendUrl << QgsWmsLegendUrlProperty();
        parseLegendUrl( e1, styleProperty.legendUrl.last() );
      }
      else if ( tagName == "StyleSheetURL" )
      {
        // TODO
      }
      else if ( tagName == "StyleURL" )
      {
        // TODO
      }
    }
    n1 = n1.nextSibling();
  }

  QgsDebugMsg( "exiting." );
}


void QgsWmsProvider::parseLayer( QDomElement const & e, QgsWmsLayerProperty& layerProperty,
                                 QgsWmsLayerProperty *parentProperty )
{
  QgsDebugMsg( "entering." );

// TODO: Delete this stanza completely, depending on success of "Inherit things into the sublayer" below.
//  // enforce WMS non-inheritance rules
//  layerProperty.name =        QString::null;
//  layerProperty.title =       QString::null;
//  layerProperty.abstract =    QString::null;
//  layerProperty.keywordList.clear();
  layerProperty.orderId     = ++mLayerCount;
  layerProperty.queryable   = e.attribute( "queryable" ).toUInt();
  layerProperty.cascaded    = e.attribute( "cascaded" ).toUInt();
  layerProperty.opaque      = e.attribute( "opaque" ).toUInt();
  layerProperty.noSubsets   = e.attribute( "noSubsets" ).toUInt();
  layerProperty.fixedWidth  = e.attribute( "fixedWidth" ).toUInt();
  layerProperty.fixedHeight = e.attribute( "fixedHeight" ).toUInt();

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QgsDebugMsg( "    "  + e1.tagName() ); // the node really is an element.

      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Layer" )
      {
        QgsDebugMsg( "      Nested layer." );

        QgsWmsLayerProperty subLayerProperty;

        // Inherit things into the sublayer
        //   Ref: 7.2.4.8 Inheritance of layer properties
        subLayerProperty.style                    = layerProperty.style;
        subLayerProperty.crs                      = layerProperty.crs;
        subLayerProperty.boundingBox              = layerProperty.boundingBox;
        subLayerProperty.ex_GeographicBoundingBox = layerProperty.ex_GeographicBoundingBox;
        // TODO

        parseLayer( e1, subLayerProperty, &layerProperty );

        layerProperty.layer.push_back( subLayerProperty );
      }
      else if ( tagName == "Name" )
      {
        layerProperty.name = e1.text();
      }
      else if ( tagName == "Title" )
      {
        layerProperty.title = e1.text();
      }
      else if ( tagName == "Abstract" )
      {
        layerProperty.abstract = e1.text();
      }
      else if ( tagName == "KeywordList" )
      {
        parseKeywordList( e1, layerProperty.keywordList );
      }
      else if ( tagName == "SRS" || tagName == "CRS" )
      {
        // CRS can contain several definitions separated by whitespace
        // though this was deprecated in WMS 1.1.1
        foreach ( QString srs, e1.text().split( QRegExp( "\\s+" ) ) )
        {
          layerProperty.crs.push_back( srs );
        }
      }
      else if ( tagName == "LatLonBoundingBox" )      // legacy from earlier versions of WMS
      {
        layerProperty.ex_GeographicBoundingBox = QgsRectangle(
              e1.attribute( "minx" ).toDouble(),
              e1.attribute( "miny" ).toDouble(),
              e1.attribute( "maxx" ).toDouble(),
              e1.attribute( "maxy" ).toDouble()
            );

        if ( e1.hasAttribute( "SRS" ) && e1.attribute( "SRS" ) != DEFAULT_LATLON_CRS )
        {
          try
          {
            QgsCoordinateReferenceSystem src;
            src.createFromOgcWmsCrs( e1.attribute( "SRS" ) );

            QgsCoordinateReferenceSystem dst;
            dst.createFromOgcWmsCrs( DEFAULT_LATLON_CRS );

            QgsCoordinateTransform ct( src, dst );
            layerProperty.ex_GeographicBoundingBox = ct.transformBoundingBox( layerProperty.ex_GeographicBoundingBox );
          }
          catch ( QgsCsException &cse )
          {
            Q_UNUSED( cse );
          }
        }
      }
      else if ( tagName == "EX_GeographicBoundingBox" ) //for WMS 1.3
      {
        QDomElement wBoundLongitudeElem, eBoundLongitudeElem, sBoundLatitudeElem, nBoundLatitudeElem;

        if ( e1.tagName() == "wms:EX_GeographicBoundingBox" )
        {
          wBoundLongitudeElem = n1.namedItem( "wms:westBoundLongitude" ).toElement();
          eBoundLongitudeElem = n1.namedItem( "wms:eastBoundLongitude" ).toElement();
          sBoundLatitudeElem = n1.namedItem( "wms:southBoundLatitude" ).toElement();
          nBoundLatitudeElem = n1.namedItem( "wms:northBoundLatitude" ).toElement();
        }
        else
        {
          wBoundLongitudeElem = n1.namedItem( "westBoundLongitude" ).toElement();
          eBoundLongitudeElem = n1.namedItem( "eastBoundLongitude" ).toElement();
          sBoundLatitudeElem = n1.namedItem( "southBoundLatitude" ).toElement();
          nBoundLatitudeElem = n1.namedItem( "northBoundLatitude" ).toElement();
        }

        double wBLong, eBLong, sBLat, nBLat;
        bool wBOk, eBOk, sBOk, nBOk;
        wBLong = wBoundLongitudeElem.text().toDouble( &wBOk );
        eBLong = eBoundLongitudeElem.text().toDouble( &eBOk );
        sBLat = sBoundLatitudeElem.text().toDouble( &sBOk );
        nBLat = nBoundLatitudeElem.text().toDouble( &nBOk );
        if ( wBOk && eBOk && sBOk && nBOk )
        {
          layerProperty.ex_GeographicBoundingBox = QgsRectangle( wBLong, sBLat, eBLong, nBLat );
        }
      }
      else if ( tagName == "BoundingBox" )
      {
        // TODO: overwrite inherited
        QgsWmsBoundingBoxProperty bbox;
        bbox.box = QgsRectangle( e1.attribute( "minx" ).toDouble(),
                                 e1.attribute( "miny" ).toDouble(),
                                 e1.attribute( "maxx" ).toDouble(),
                                 e1.attribute( "maxy" ).toDouble()
                               );
        if ( e1.hasAttribute( "CRS" ) || e1.hasAttribute( "SRS" ) )
        {
          if ( e1.hasAttribute( "CRS" ) )
            bbox.crs = e1.attribute( "CRS" );
          else if ( e1.hasAttribute( "SRS" ) )
            bbox.crs = e1.attribute( "SRS" );

          layerProperty.boundingBox.push_back( bbox );
        }
        else
        {
          QgsDebugMsg( "CRS/SRS attribute not found in BoundingBox" );
        }
      }
      else if ( tagName == "Dimension" )
      {
        // TODO
      }
      else if ( tagName == "Attribution" )
      {
        // TODO
      }
      else if ( tagName == "AuthorityURL" )
      {
        // TODO
      }
      else if ( tagName == "Identifier" )
      {
        // TODO
      }
      else if ( tagName == "MetadataURL" )
      {
        // TODO
      }
      else if ( tagName == "DataURL" )
      {
        // TODO
      }
      else if ( tagName == "FeatureListURL" )
      {
        // TODO
      }
      else if ( tagName == "Style" )
      {
        QgsWmsStyleProperty styleProperty;

        parseStyle( e1, styleProperty );

        layerProperty.style.push_back( styleProperty );
      }
      else if ( tagName == "MinScaleDenominator" )
      {
        // TODO
      }
      else if ( tagName == "MaxScaleDenominator" )
      {
        // TODO
      }
      // If we got here then it's not in the WMS 1.3 standard

    }
    n1 = n1.nextSibling();
  }

  if ( parentProperty )
  {
    mLayerParents[ layerProperty.orderId ] = parentProperty->orderId;
  }

  if ( !layerProperty.name.isEmpty() )
  {
    // We have all the information we need to properly evaluate a layer definition
    // TODO: Save this somewhere

    // Store if the layer is queryable
    mQueryableForLayer[ layerProperty.name ] = layerProperty.queryable;

    // Store the available Coordinate Reference Systems for the layer so that it
    // can be combined with others later in supportedCrsForLayers()
    mCrsForLayer[ layerProperty.name ] = layerProperty.crs;

    // Insert into the local class' registry
    mLayersSupported.push_back( layerProperty );

    //if there are several <Layer> elements without a parent layer, the style list needs to be cleared
    if ( layerProperty.layer.empty() )
    {
      layerProperty.style.clear();
    }
  }

  if ( !layerProperty.layer.empty() )
  {
    mLayerParentNames[ layerProperty.orderId ] = QStringList() << layerProperty.name << layerProperty.title << layerProperty.abstract;
  }

  if ( !parentProperty )
  {
    // Why clear()? I need top level access. Seems to work in standard select dialog without clear.
    //layerProperty.layer.clear();
    layerProperty.crs.clear();
  }

  QgsDebugMsg( "exiting." );
}


static const QgsWmsLayerProperty* _findNestedLayerProperty( const QString& layerName, const QgsWmsLayerProperty* prop )
{
  if ( prop->name == layerName )
    return prop;

  foreach ( const QgsWmsLayerProperty& child, prop->layer )
  {
    if ( const QgsWmsLayerProperty* res = _findNestedLayerProperty( layerName, &child ) )
      return res;
  }

  return 0;
}


bool QgsWmsProvider::extentForNonTiledLayer( const QString& layerName, const QString& crs, QgsRectangle& extent )
{
  const QgsWmsLayerProperty* layerProperty = _findNestedLayerProperty( layerName, &mCapabilities.capability.layer );
  if ( !layerProperty )
    return false;

  // see if we can refine the bounding box with the CRS-specific bounding boxes
  for ( int i = 0; i < layerProperty->boundingBox.size(); i++ )
  {
    if ( layerProperty->boundingBox[i].crs == crs )
    {
      // exact bounding box is provided for this CRS
      extent = layerProperty->boundingBox[i].box;
      return true;
    }
  }

  // exact bounding box for given CRS is not listed - we need to pick a different
  // bounding box definition - either the coarse bounding box (in WGS84)
  // or one of the alternative bounding box definitions for the layer

  // Use the coarse bounding box
  extent = layerProperty->ex_GeographicBoundingBox;

  for ( int i = 0; i < layerProperty->boundingBox.size(); i++ )
  {
    if ( layerProperty->boundingBox[i].crs == DEFAULT_LATLON_CRS )
    {
      if ( layerProperty->boundingBox[i].box.contains( extent ) )
        continue; // this bounding box is less specific (probably inherited from parent)

      // this BBox is probably better than the one in ex_GeographicBoundingBox
      extent = layerProperty->boundingBox[i].box;
      break;
    }
  }

  // transform it to requested CRS

  QgsCoordinateReferenceSystem dst, wgs;
  if ( !wgs.createFromOgcWmsCrs( DEFAULT_LATLON_CRS ) || !dst.createFromOgcWmsCrs( crs ) )
    return false;

  QgsCoordinateTransform xform( wgs, dst );
  QgsDebugMsg( QString( "transforming layer extent %1" ).arg( extent.toString( true ) ) );
  try
  {
    extent = xform.transformBoundingBox( extent );
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse );
    return false;
  }
  QgsDebugMsg( QString( "transformed layer extent %1" ).arg( extent.toString( true ) ) );

  //make sure extent does not contain 'inf' or 'nan'
  if ( !extent.isFinite() )
  {
    return false;
  }

  return true;
}


void QgsWmsProvider::parseTileSetProfile( QDomElement const &e )
{
  QStringList resolutions, layers, styles;
  QgsWmtsTileMatrixSet ms;
  QgsWmtsTileMatrix m;
  QgsWmtsTileLayer l;

  l.tileMode = WMSC;

  QDomNode n1 = e.firstChild();
  while ( !n1.isNull() )
  {
    QDomElement e1 = n1.toElement(); // try to convert the node to an element.
    if ( !e1.isNull() )
    {
      QgsDebugMsg( "    "  + e1.tagName() ); // the node really is an element.

      QString tagName = e1.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "Layers" )
      {
        layers << e1.text();
      }
      else if ( tagName == "Styles" )
      {
        styles << e1.text();
      }
      else if ( tagName == "Width" )
      {
        m.tileWidth = e1.text().toInt();
      }
      else if ( tagName == "Height" )
      {
        m.tileHeight = e1.text().toInt();
      }
      else if ( tagName == "SRS" )
      {
        ms.crs = e1.text();
      }
      else if ( tagName == "Format" )
      {
        l.formats << e1.text();
      }
      else if ( tagName == "BoundingBox" )
      {
        l.boundingBox.box = QgsRectangle(
                              e1.attribute( "minx" ).toDouble(),
                              e1.attribute( "miny" ).toDouble(),
                              e1.attribute( "maxx" ).toDouble(),
                              e1.attribute( "maxy" ).toDouble()
                            );
        if ( e1.hasAttribute( "SRS" ) )
          l.boundingBox.crs = e1.attribute( "SRS" );
        else if ( e1.hasAttribute( "srs" ) )
          l.boundingBox.crs = e1.attribute( "srs" );
        else if ( e1.hasAttribute( "CRS" ) )
          l.boundingBox.crs = e1.attribute( "CRS" );
        else if ( e1.hasAttribute( "crs" ) )
          l.boundingBox.crs = e1.attribute( "crs" );
      }
      else if ( tagName == "Resolutions" )
      {
        resolutions = e1.text().trimmed().split( " ", QString::SkipEmptyParts );
      }
      else
      {
        QgsDebugMsg( QString( "tileset tag %1 ignored" ).arg( e1.tagName() ) );
      }
    }
    n1 = n1.nextSibling();
  }

  ms.identifier = QString( "%1-wmsc-%2" ).arg( layers.join( "_" ) ).arg( mTileLayersSupported.size() );

  l.identifier = layers.join( "," );
  QgsWmtsStyle s;
  s.identifier = styles.join( "," );
  l.styles.insert( s.identifier, s );
  l.defaultStyle = s.identifier;

  QgsWmtsTileMatrixSetLink sl;
  sl.tileMatrixSet = ms.identifier;
  l.setLinks.insert( ms.identifier, sl );
  mTileLayersSupported.append( l );

  int i = 0;
  foreach ( QString rS, resolutions )
  {
    double r = rS.toDouble();
    m.identifier = QString::number( i );
    m.matrixWidth  = ceil( l.boundingBox.box.width() / m.tileWidth / r );
    m.matrixHeight = ceil( l.boundingBox.box.height() / m.tileHeight / r );
    m.topLeft = QgsPoint( l.boundingBox.box.xMinimum(), l.boundingBox.box.yMinimum() + m.matrixHeight * m.tileHeight * r );
    ms.tileMatrices.insert( r, m );
    i++;
  }

  mTileMatrixSets.insert( ms.identifier, ms );
}

void QgsWmsProvider::parseKeywords( const QDomNode &e, QStringList &keywords )
{
  keywords.clear();

  for ( QDomElement e1 = e.firstChildElement( "ows:Keywords" ).firstChildElement( "ows:Keyword" );
        !e1.isNull();
        e1 = e1.nextSiblingElement( "ows:Keyword" ) )
  {
    keywords << e1.text();
  }
}

void QgsWmsProvider::parseTheme( const QDomElement &e, QgsWmtsTheme &t )
{
  t.identifier = e.firstChildElement( "ows:Identifier" ).text();
  t.title      = e.firstChildElement( "ows:Title" ).text();
  t.abstract   = e.firstChildElement( "ows:Abstract" ).text();
  parseKeywords( e, t.keywords );

  QDomElement sl = e.firstChildElement( "ows:Theme" );
  if ( !sl.isNull() )
  {
    t.subTheme = new QgsWmtsTheme;
    parseTheme( sl, *t.subTheme );
  }
  else
  {
    t.subTheme = 0;
  }

  t.layerRefs.clear();
  for ( QDomElement e1 = e.firstChildElement( "ows:LayerRef" );
        !e1.isNull();
        e1 = e1.nextSiblingElement( "ows:LayerRef" ) )
  {
    t.layerRefs << e1.text();
  }
}

void QgsWmsProvider::parseWMTSContents( QDomElement const &e )
{
  //
  // tile matrix sets
  //

  mTileMatrixSets.clear();
  for ( QDomNode n0 = e.firstChildElement( "TileMatrixSet" ); !n0.isNull(); n0 = n0.nextSiblingElement( "TileMatrixSet" ) )
  {
    QgsWmtsTileMatrixSet s;
    s.identifier = n0.firstChildElement( "ows:Identifier" ).text();
    s.title      = n0.firstChildElement( "ows:Title" ).text();
    s.abstract   = n0.firstChildElement( "ows:Abstract" ).text();
    parseKeywords( n0, s.keywords );

    QString supportedCRS = n0.firstChildElement( "ows:SupportedCRS" ).text();

    QgsCoordinateReferenceSystem crs;
    crs.createFromOgcWmsCrs( supportedCRS );

    s.wkScaleSet = n0.firstChildElement( "WellKnownScaleSet" ).text();

    double metersPerUnit = QGis::fromUnitToUnitFactor( crs.mapUnits(), QGis::Meters );

    s.crs = crs.authid();

    bool invert = !mIgnoreAxisOrientation && crs.axisInverted();
    if ( mInvertAxisOrientation )
      invert = !invert;

    QgsDebugMsg( QString( "tilematrix set: %1 (supportedCRS:%2 crs:%3; metersPerUnit:%4 axisInverted:%5)" )
                 .arg( s.identifier )
                 .arg( supportedCRS )
                 .arg( s.crs )
                 .arg( metersPerUnit, 0, 'f' )
                 .arg( invert ? "yes" : "no" )
               );

    for ( QDomNode n1 = n0.firstChildElement( "TileMatrix" );
          !n1.isNull();
          n1 = n1.nextSiblingElement( "TileMatrix" ) )
    {
      QgsWmtsTileMatrix m;

      m.identifier = n1.firstChildElement( "ows:Identifier" ).text();
      m.title      = n1.firstChildElement( "ows:Title" ).text();
      m.abstract   = n1.firstChildElement( "ows:Abstract" ).text();
      parseKeywords( n1, m.keywords );

      m.scaleDenom = n1.firstChildElement( "ScaleDenominator" ).text().toDouble();

      QStringList topLeft = n1.firstChildElement( "TopLeftCorner" ).text().split( " " );
      if ( topLeft.size() == 2 )
      {
        if ( invert )
        {
          m.topLeft.set( topLeft[1].toDouble(), topLeft[0].toDouble() );
        }
        else
        {
          m.topLeft.set( topLeft[0].toDouble(), topLeft[1].toDouble() );
        }
      }
      else
      {
        QgsDebugMsg( "Could not parse topLeft" );
        continue;
      }

      m.tileWidth    = n1.firstChildElement( "TileWidth" ).text().toInt();
      m.tileHeight   = n1.firstChildElement( "TileHeight" ).text().toInt();
      m.matrixWidth  = n1.firstChildElement( "MatrixWidth" ).text().toInt();
      m.matrixHeight = n1.firstChildElement( "MatrixHeight" ).text().toInt();

      double res = m.scaleDenom * 0.00028 / metersPerUnit;

      QgsDebugMsg( QString( " %1: scale=%2 res=%3 tile=%4x%5 matrix=%6x%7 topLeft=%8" )
                   .arg( m.identifier )
                   .arg( m.scaleDenom ).arg( res )
                   .arg( m.tileWidth ).arg( m.tileHeight )
                   .arg( m.matrixWidth ).arg( m.matrixHeight )
                   .arg( m.topLeft.toString() )
                 );

      s.tileMatrices.insert( res, m );
    }

    mTileMatrixSets.insert( s.identifier, s );
  }

  //
  // layers
  //

  mTileLayersSupported.clear();
  for ( QDomElement e0 = e.firstChildElement( "Layer" );
        !e0.isNull();
        e0 = e0.nextSiblingElement( "Layer" ) )
  {
    QString id = e0.firstChildElement( "ows:Identifier" ).text();
    QgsDebugMsg( QString( "Layer %1" ).arg( id ) );

    QgsWmtsTileLayer l;
    l.tileMode   = WMTS;
    l.identifier = e0.firstChildElement( "ows:Identifier" ).text();
    l.title      = e0.firstChildElement( "ows:Title" ).text();
    l.abstract   = e0.firstChildElement( "ows:Abstract" ).text();
    parseKeywords( e0, l.keywords );

    l.boundingBox.crs = "";

    QDomElement bbox = e0.firstChildElement( "ows:WGS84BoundingBox" );
    if ( !bbox.isNull() )
    {
      QStringList ll = bbox.firstChildElement( "ows:LowerCorner" ).text().split( " " );
      QStringList ur = bbox.firstChildElement( "ows:UpperCorner" ).text().split( " " );

      if ( ll.size() == 2 && ur.size() == 2 )
      {
        l.boundingBox.box = QgsRectangle( QgsPoint( ll[0].toDouble(), ll[1].toDouble() ),
                                          QgsPoint( ur[0].toDouble(), ur[1].toDouble() ) );
        l.boundingBox.crs = DEFAULT_LATLON_CRS;
      }
    }

    if ( l.boundingBox.crs.isEmpty() )
    {
      bbox = e0.firstChildElement( "ows:BoundingBox" );
      if ( !bbox.isNull() )
      {
        QStringList ll = bbox.firstChildElement( "ows:LowerCorner" ).text().split( " " );
        QStringList ur = bbox.firstChildElement( "ows:UpperCorner" ).text().split( " " );

        if ( ll.size() == 2 && ur.size() == 2 )
        {
          l.boundingBox.box = QgsRectangle( QgsPoint( ll[0].toDouble(), ll[1].toDouble() ),
                                            QgsPoint( ur[0].toDouble(), ur[1].toDouble() ) );

          if ( bbox.hasAttribute( "SRS" ) )
            l.boundingBox.crs = bbox.attribute( "SRS" );
          else if ( bbox.hasAttribute( "srs" ) )
            l.boundingBox.crs = bbox.attribute( "srs" );
          else if ( bbox.hasAttribute( "CRS" ) )
            l.boundingBox.crs = bbox.attribute( "CRS" );
          else if ( bbox.hasAttribute( "crs" ) )
            l.boundingBox.crs = bbox.attribute( "crs" );
        }
      }
    }

    for ( QDomElement e1 = e0.firstChildElement( "Style" );
          !e1.isNull();
          e1 = e1.nextSiblingElement( "Style" ) )
    {
      QgsWmtsStyle s;
      s.identifier = e1.firstChildElement( "ows:Identifier" ).text();
      s.title      = e1.firstChildElement( "ows:Title" ).text();
      s.abstract   = e1.firstChildElement( "ows:Abstract" ).text();
      parseKeywords( e1, s.keywords );

      for ( QDomElement e2 = e1.firstChildElement( "ows:legendURL" );
            !e2.isNull();
            e2 = e2.nextSiblingElement( "ows:legendURL" ) )
      {
        QgsWmtsLegendURL u;

        u.format   = e2.firstChildElement( "format" ).text();
        u.minScale = e2.firstChildElement( "minScale" ).text().toDouble();
        u.maxScale = e2.firstChildElement( "maxScale" ).text().toDouble();
        u.href     = e2.firstChildElement( "href" ).text();
        u.width    = e2.firstChildElement( "width" ).text().toInt();
        u.height   = e2.firstChildElement( "height" ).text().toInt();

        s.legendURLs << u;
      }

      s.isDefault = e1.attribute( "isDefault" ) == "true";

      l.styles.insert( s.identifier, s );

      if ( s.isDefault )
        l.defaultStyle = s.identifier;
    }

    if ( l.styles.isEmpty() )
    {
      QgsWmtsStyle s;
      s.identifier = "default";
      s.title      = tr( "Generated default style" );
      s.abstract   = tr( "Style was missing in capabilities" );
      l.styles.insert( s.identifier, s );
    }

    for ( QDomElement e1 = e0.firstChildElement( "Format" ); !e1.isNull(); e1 = e1.nextSiblingElement( "Format" ) )
    {
      l.formats << e1.text();
    }

    for ( QDomElement e1 = e0.firstChildElement( "InfoFormat" ); !e1.isNull(); e1 = e1.nextSiblingElement( "InfoFormat" ) )
    {
      l.infoFormats << e1.text();
    }

    for ( QDomElement e1 = e0.firstChildElement( "Dimension" ); !e1.isNull(); e1 = e1.nextSiblingElement( "Dimension" ) )
    {
      QgsWmtsDimension d;

      d.identifier   = e1.firstChildElement( "ows:Identifier" ).text();
      if ( d.identifier.isEmpty() )
        continue;

      d.title        = e1.firstChildElement( "ows:Title" ).text();
      d.abstract     = e1.firstChildElement( "ows:Abstract" ).text();
      parseKeywords( e1, d.keywords );

      d.UOM          = e1.firstChildElement( "UOM" ).text();
      d.unitSymbol   = e1.firstChildElement( "unitSymbol" ).text();
      d.defaultValue = e1.firstChildElement( "Default" ).text();
      d.current      = e1.firstChildElement( "current" ).text() == "true";

      for ( QDomElement e2 = e1.firstChildElement( "Value" );
            !e2.isNull();
            e2 = e2.nextSiblingElement( "Value" ) )
      {
        d.values << e2.text();
      }

      l.dimensions.insert( d.identifier, d );
    }

    for ( QDomElement e1 = e0.firstChildElement( "TileMatrixSetLink" ); !e1.isNull(); e1 = e1.nextSiblingElement( "TileMatrixSetLink" ) )
    {
      QgsWmtsTileMatrixSetLink sl;

      sl.tileMatrixSet = e1.firstChildElement( "TileMatrixSet" ).text();

      if ( !mTileMatrixSets.contains( sl.tileMatrixSet ) )
      {
        QgsDebugMsg( QString( "  TileMatrixSet %1 not found." ).arg( sl.tileMatrixSet ) );
        continue;
      }

      const QgsWmtsTileMatrixSet &tms = mTileMatrixSets[ sl.tileMatrixSet ];

      for ( QDomElement e2 = e1.firstChildElement( "TileMatrixSetLimits" ); !e2.isNull(); e2 = e2.nextSiblingElement( "TileMatrixSetLimits" ) )
      {
        for ( QDomElement e3 = e2.firstChildElement( "TileMatrixLimits" ); !e3.isNull(); e3 = e3.nextSiblingElement( "TileMatrixLimits" ) )
        {
          QgsWmtsTileMatrixLimits limit;

          QString id = e3.firstChildElement( "TileMatrix" ).text();

          bool isValid = false;
          int matrixWidth = -1, matrixHeight = -1;
          foreach ( const QgsWmtsTileMatrix &m, tms.tileMatrices )
          {
            isValid = m.identifier == id;
            if ( isValid )
            {
              matrixWidth = m.matrixWidth;
              matrixHeight = m.matrixHeight;
              break;
            }
          }

          if ( isValid )
          {
            limit.minTileRow = e3.firstChildElement( "MinTileRow" ).text().toInt();
            limit.maxTileRow = e3.firstChildElement( "MaxTileRow" ).text().toInt();
            limit.minTileCol = e3.firstChildElement( "MinTileCol" ).text().toInt();
            limit.maxTileCol = e3.firstChildElement( "MaxTileCol" ).text().toInt();

            isValid =
              limit.minTileCol >= 0 && limit.minTileCol < matrixWidth &&
              limit.maxTileCol >= 0 && limit.maxTileCol < matrixWidth &&
              limit.minTileCol <= limit.maxTileCol &&
              limit.minTileRow >= 0 && limit.minTileRow < matrixHeight &&
              limit.maxTileRow >= 0 && limit.maxTileRow < matrixHeight &&
              limit.minTileRow <= limit.maxTileRow;
          }
          else
          {
            QgsDebugMsg( QString( "   TileMatrix id:%1 not found." ).arg( id ) );
          }

          QgsDebugMsg( QString( "   TileMatrixLimit id:%1 row:%2-%3 col:%4-%5 matrix:%6x%7 %8" )
                       .arg( id )
                       .arg( limit.minTileRow ).arg( limit.maxTileRow )
                       .arg( limit.minTileCol ).arg( limit.maxTileCol )
                       .arg( matrixWidth ).arg( matrixHeight )
                       .arg( isValid ? "valid" : "INVALID" )
                     );

          if ( isValid )
          {
            sl.limits.insert( id, limit );
          }
        }
      }

      l.setLinks.insert( sl.tileMatrixSet, sl );
    }

    for ( QDomElement e1 = e0.firstChildElement( "ResourceURL" ); !e1.isNull(); e1 = e1.nextSiblingElement( "ResourceURL" ) )
    {
      QString format       = nodeAttribute( e1, "format" );
      QString resourceType = nodeAttribute( e1, "resourceType" );
      QString tmpl         = nodeAttribute( e1, "template" );

      if ( format.isEmpty() || resourceType.isEmpty() || tmpl.isEmpty() )
      {
        QgsDebugMsg( QString( "SKIPPING ResourceURL format=%1 resourceType=%2 template=%3" )
                     .arg( format )
                     .arg( resourceType )
                     .arg( tmpl ) ) ;
        continue;
      }

      if ( resourceType == "tile" )
      {
        l.getTileURLs.insert( format, tmpl );
      }
      else if ( resourceType == "FeatureInfo" )
      {
        l.getFeatureInfoURLs.insert( format, tmpl );
      }
      else
      {
        QgsDebugMsg( QString( "UNEXPECTED resourceType in ResourcURL format=%1 resourceType=%2 template=%3" )
                     .arg( format )
                     .arg( resourceType )
                     .arg( tmpl ) ) ;
      }
    }

    QgsDebugMsg( QString( "add layer %1" ).arg( id ) );
    mTileLayersSupported << l;
  }

  //
  // themes
  //
  mTileThemes.clear();
  for ( QDomElement e0 = e.firstChildElement( "Themes" ).firstChildElement( "Theme" );
        !e0.isNull();
        e0 = e0.nextSiblingElement( "Theme" ) )
  {
    mTileThemes << QgsWmtsTheme();
    parseTheme( e0, mTileThemes.back() );
  }

  // make sure that all layers have a bounding box
  for ( QList<QgsWmtsTileLayer>::iterator it = mTileLayersSupported.begin(); it != mTileLayersSupported.end(); ++it )
  {
    QgsWmtsTileLayer& l = *it;

    if ( l.boundingBox.crs.isEmpty() )
    {
      if ( !detectTileLayerBoundingBox( l ) )
      {
        QgsDebugMsg( "failed to detect bounding box for " + l.identifier + " - using extent of the whole world" );
        l.boundingBox.box = QgsRectangle( -180.0, -90.0, 180.0, 90.0 );
        l.boundingBox.crs = DEFAULT_LATLON_CRS;
      }
    }
  }
}


bool QgsWmsProvider::detectTileLayerBoundingBox( QgsWmtsTileLayer& l )
{
  if ( l.setLinks.isEmpty() )
    return false;

  // take first supported tile matrix set
  const QgsWmtsTileMatrixSetLink& setLink = l.setLinks.constBegin().value();

  QHash<QString, QgsWmtsTileMatrixSet>::const_iterator tmsIt = mTileMatrixSets.constFind( setLink.tileMatrixSet );
  if ( tmsIt == mTileMatrixSets.constEnd() )
    return false;

  QgsCoordinateReferenceSystem crs;
  if ( !crs.createFromOgcWmsCrs( tmsIt->crs ) )
    return false;

  // take most coarse tile matrix ...
  QMap<double, QgsWmtsTileMatrix>::const_iterator tmIt = tmsIt->tileMatrices.constEnd() - 1;
  if ( tmIt == tmsIt->tileMatrices.constEnd() )
    return false;

  const QgsWmtsTileMatrix& tm = *tmIt;
  double metersPerUnit = QGis::fromUnitToUnitFactor( crs.mapUnits(), QGis::Meters );
  double res = tm.scaleDenom * 0.00028 / metersPerUnit;
  QgsPoint bottomRight( tm.topLeft.x() + res * tm.tileWidth * tm.matrixWidth,
                        tm.topLeft.y() - res * tm.tileHeight * tm.matrixHeight );

  QgsDebugMsg( QString( "detecting WMTS layer bounding box: tileset %1 matrix %2 crs %3 res %4" )
               .arg( tmsIt->identifier ).arg( tm.identifier ).arg( tmsIt->crs ).arg( res ) );

  QgsRectangle extent( tm.topLeft, bottomRight );
  extent.normalize();

  l.boundingBox.box = extent;
  l.boundingBox.crs = tmsIt->crs;
  return true;
}


void QgsWmsProvider::layerParents( QMap<int, int> &parents, QMap<int, QStringList> &parentNames ) const
{
  parents = mLayerParents;
  parentNames = mLayerParentNames;
}

bool QgsWmsProvider::parseServiceExceptionReportDom( QByteArray const & xml )
{
  QgsDebugMsg( "entering." );

#ifdef QGISDEBUG
  //test the content of the QByteArray
  QString responsestring( xml );
  QgsDebugMsg( "received the following data: " + responsestring );
#endif

  // Convert completed document into a Dom
  QString errorMsg;
  int errorLine;
  int errorColumn;
  bool contentSuccess = mServiceExceptionReportDom.setContent( xml, false, &errorMsg, &errorLine, &errorColumn );

  if ( !contentSuccess )
  {
    mErrorCaption = tr( "Dom Exception" );
    mErrorFormat = "text/plain";
    mError = tr( "Could not get WMS Service Exception at %1: %2 at line %3 column %4\n\nResponse was:\n\n%5" )
             .arg( mBaseUrl )
             .arg( errorMsg )
             .arg( errorLine )
             .arg( errorColumn )
             .arg( QString( xml ) );

    QgsLogger::debug( "Dom Exception: " + mError );

    return false;
  }

  QDomElement docElem = mServiceExceptionReportDom.documentElement();

  // TODO: Assert the docElem.tagName() is "ServiceExceptionReport"

  // serviceExceptionProperty.version = docElem.attribute("version");

  // Start walking through XML.
  QDomNode n = docElem.firstChild();

  while ( !n.isNull() )
  {
    QDomElement e = n.toElement(); // try to convert the node to an element.
    if ( !e.isNull() )
    {
      QgsDebugMsg( e.tagName() ); // the node really is an element.

      QString tagName = e.tagName();
      if ( tagName.startsWith( "wms:" ) )
        tagName = tagName.mid( 4 );

      if ( tagName == "ServiceException" )
      {
        QgsDebugMsg( "  ServiceException." );
        parseServiceException( e );
      }

    }
    n = n.nextSibling();
  }

  QgsDebugMsg( "exiting." );

  return true;
}


void QgsWmsProvider::parseServiceException( QDomElement const & e )
{
  QgsDebugMsg( "entering." );

  QString seCode = e.attribute( "code" );
  QString seText = e.text();

  mErrorCaption = tr( "Service Exception" );
  mErrorFormat = "text/plain";

  // set up friendly descriptions for the service exception
  if ( seCode == "InvalidFormat" )
  {
    mError = tr( "Request contains a format not offered by the server." );
  }
  else if ( seCode == "InvalidCRS" )
  {
    mError = tr( "Request contains a CRS not offered by the server for one or more of the Layers in the request." );
  }
  else if ( seCode == "InvalidSRS" )  // legacy WMS < 1.3.0
  {
    mError = tr( "Request contains a SRS not offered by the server for one or more of the Layers in the request." );
  }
  else if ( seCode == "LayerNotDefined" )
  {
    mError = tr( "GetMap request is for a Layer not offered by the server, "
                 "or GetFeatureInfo request is for a Layer not shown on the map." );
  }
  else if ( seCode == "StyleNotDefined" )
  {
    mError = tr( "Request is for a Layer in a Style not offered by the server." );
  }
  else if ( seCode == "LayerNotQueryable" )
  {
    mError = tr( "GetFeatureInfo request is applied to a Layer which is not declared queryable." );
  }
  else if ( seCode == "InvalidPoint" )
  {
    mError = tr( "GetFeatureInfo request contains invalid X or Y value." );
  }
  else if ( seCode == "CurrentUpdateSequence" )
  {
    mError = tr( "Value of (optional) UpdateSequence parameter in GetCapabilities request is equal to "
                 "current value of service metadata update sequence number." );
  }
  else if ( seCode == "InvalidUpdateSequence" )
  {
    mError = tr( "Value of (optional) UpdateSequence parameter in GetCapabilities request is greater "
                 "than current value of service metadata update sequence number." );
  }
  else if ( seCode == "MissingDimensionValue" )
  {
    mError = tr( "Request does not include a sample dimension value, and the server did not declare a "
                 "default value for that dimension." );
  }
  else if ( seCode == "InvalidDimensionValue" )
  {
    mError = tr( "Request contains an invalid sample dimension value." );
  }
  else if ( seCode == "OperationNotSupported" )
  {
    mError = tr( "Request is for an optional operation that is not supported by the server." );
  }
  else if ( seCode.isEmpty() )
  {
    mError = tr( "(No error code was reported)" );
  }
  else
  {
    mError = seCode + " " + tr( "(Unknown error code)" );
  }

  mError += "\n" + tr( "The WMS vendor also reported: " );
  mError += seText;

  // TODO = e.attribute("locator");

  QgsDebugMsg( QString( "exiting with composed error message '%1'." ).arg( mError ) );
}

QgsRectangle QgsWmsProvider::extent()
{
  if ( mExtentDirty )
  {
    if ( calculateExtent() )
    {
      mExtentDirty = false;
    }
  }

  return mLayerExtent;
}

bool QgsWmsProvider::isValid()
{
  return mValid;
}


QString QgsWmsProvider::wmsVersion()
{
  // TODO
  return NULL;
}

QStringList QgsWmsProvider::supportedImageEncodings()
{
  return mCapabilities.capability.request.getMap.format;
}


QStringList QgsWmsProvider::subLayers() const
{
  return mActiveSubLayers;
}


QStringList QgsWmsProvider::subLayerStyles() const
{
  return mActiveSubStyles;
}

bool QgsWmsProvider::calculateExtent()
{
  //! \todo Make this handle non-geographic CRSs (e.g. floor plans) as per WMS spec

  QgsDebugMsg( "entered." );

  // Make sure we know what extents are available
  if ( !retrieveServerCapabilities() )
  {
    return false;
  }

  // Set up the coordinate transform from the WMS standard CRS:84 bounding
  // box to the user's selected CRS
  if ( !mCoordinateTransform )
  {
    QgsCoordinateReferenceSystem qgisSrsSource;
    QgsCoordinateReferenceSystem qgisSrsDest;

    if ( mTiled && mTileLayer )
    {
      QgsDebugMsg( QString( "Tile layer's extent: %1 %2" ).arg( mTileLayer->boundingBox.box.toString() ).arg( mTileLayer->boundingBox.crs ) );
      qgisSrsSource.createFromOgcWmsCrs( mTileLayer->boundingBox.crs );
    }
    else
    {
      qgisSrsSource.createFromOgcWmsCrs( DEFAULT_LATLON_CRS );
    }

    qgisSrsDest.createFromOgcWmsCrs( mImageCrs );

    mCoordinateTransform = new QgsCoordinateTransform( qgisSrsSource, qgisSrsDest );
  }

  if ( mTiled )
  {
    if ( mTileLayer )
    {
      try
      {
        QgsRectangle extent = mCoordinateTransform->transformBoundingBox( mTileLayer->boundingBox.box, QgsCoordinateTransform::ForwardTransform );

        //make sure extent does not contain 'inf' or 'nan'
        if ( extent.isFinite() )
        {
          QgsDebugMsg( "exiting with '"  + mLayerExtent.toString() + "'." );
          mLayerExtent = extent;
          return true;
        }
      }
      catch ( QgsCsException &cse )
      {
        Q_UNUSED( cse );
      }
    }

    QgsDebugMsg( "no extent returned" );
    return false;
  }
  else
  {
    bool firstLayer = true; //flag to know if a layer is the first to be successfully transformed
    for ( QStringList::Iterator it  = mActiveSubLayers.begin();
          it != mActiveSubLayers.end();
          ++it )
    {
      QgsDebugMsg( "Sublayer iterator: " + *it );

      QgsRectangle extent;
      if ( !extentForNonTiledLayer( *it, mImageCrs, extent ) )
      {
        QgsDebugMsg( "extent for " + *it + " is invalid! (ignoring)" );
        continue;
      }

      QgsDebugMsg( "extent for " + *it  + " is " + extent.toString( 3 )  + "." );

      // add to the combined extent of all the active sublayers
      if ( firstLayer )
      {
        mLayerExtent = extent;
      }
      else
      {
        mLayerExtent.combineExtentWith( &extent );
      }

      firstLayer = false;

      QgsDebugMsg( "combined extent is '"  + mLayerExtent.toString()
                   + "' after '"  + ( *it ) + "'." );

    }

    QgsDebugMsg( "exiting with '"  + mLayerExtent.toString() + "'." );
    return true;
  }
}


int QgsWmsProvider::capabilities() const
{
  int capability = NoCapabilities;
  bool canIdentify = false;

  QgsDebugMsg( "entering." );

  // Test for the ability to use the Identify map tool
  for ( QStringList::const_iterator it = mActiveSubLayers.begin();
        it != mActiveSubLayers.end();
        ++it )
  {
    // Is sublayer visible?
    if ( mActiveSubLayerVisibility.find( *it ).value() )
    {
      // Is sublayer queryable?
      if ( mQueryableForLayer.find( *it ).value() )
      {
        QgsDebugMsg( "'"  + ( *it )  + "' is queryable." );
        canIdentify = true;
      }
    }
  }

  if ( canIdentify )
  {
    if ( identifyCapabilities() )
    {
      capability |= identifyCapabilities() | Identify;
    }
  }
  QgsDebugMsg( QString( "capability = %1" ).arg( capability ) );
  return capability;
}

int QgsWmsProvider::identifyCapabilities() const
{
  int capability = NoCapabilities;

  foreach ( QgsRaster::IdentifyFormat f, mIdentifyFormats.keys() )
  {
    capability |= identifyFormatToCapability( f );
  }

  QgsDebugMsg( QString( "capability = %1" ).arg( capability ) );
  return capability;
}

QString QgsWmsProvider::layerMetadata( QgsWmsLayerProperty &layer )
{
  QString metadata;

  // Layer Properties section

  // Use a nested table
  metadata += "<tr><td>";
  metadata += "<table width=\"100%\">";

  // Table header
  metadata += "<tr><th class=\"glossy\">";
  metadata += tr( "Property" );
  metadata += "</th>";
  metadata += "<th class=\"glossy\">";
  metadata += tr( "Value" );
  metadata += "</th></tr>";

  // Name
  metadata += "<tr><td>";
  metadata += tr( "Name" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.name;
  metadata += "</td></tr>";

  // Layer Visibility (as managed by this provider)
  metadata += "<tr><td>";
  metadata += tr( "Visibility" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mActiveSubLayerVisibility.find( layer.name ).value() ? tr( "Visible" ) : tr( "Hidden" );
  metadata += "</td></tr>";

  // Layer Title
  metadata += "<tr><td>";
  metadata += tr( "Title" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.title;
  metadata += "</td></tr>";

  // Layer Abstract
  metadata += "<tr><td>";
  metadata += tr( "Abstract" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.abstract;
  metadata += "</td></tr>";

  // Layer Queryability
  metadata += "<tr><td>";
  metadata += tr( "Can Identify" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.queryable ? tr( "Yes" ) : tr( "No" );
  metadata += "</td></tr>";

  // Layer Opacity
  metadata += "<tr><td>";
  metadata += tr( "Can be Transparent" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.opaque ? tr( "No" ) : tr( "Yes" );
  metadata += "</td></tr>";

  // Layer Subsetability
  metadata += "<tr><td>";
  metadata += tr( "Can Zoom In" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += layer.noSubsets ? tr( "No" ) : tr( "Yes" );
  metadata += "</td></tr>";

  // Layer Server Cascade Count
  metadata += "<tr><td>";
  metadata += tr( "Cascade Count" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += QString::number( layer.cascaded );
  metadata += "</td></tr>";

  // Layer Fixed Width
  metadata += "<tr><td>";
  metadata += tr( "Fixed Width" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += QString::number( layer.fixedWidth );
  metadata += "</td></tr>";

  // Layer Fixed Height
  metadata += "<tr><td>";
  metadata += tr( "Fixed Height" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += QString::number( layer.fixedHeight );
  metadata += "</td></tr>";

  // Layer Coordinate Reference Systems
  for ( int j = 0; j < qMin( layer.crs.size(), 10 ); j++ )
  {
    metadata += "<tr><td>";
    metadata += tr( "Available in CRS" );
    metadata += "</td>";
    metadata += "<td>";
    metadata += layer.crs[j];
    metadata += "</td></tr>";
  }

  if ( layer.crs.size() > 10 )
  {
    metadata += "<tr><td>";
    metadata += tr( "Available in CRS" );
    metadata += "</td>";
    metadata += "<td>";
    metadata += tr( "(and %n more)", "crs", layer.crs.size() - 10 );
    metadata += "</td></tr>";
  }

  // Layer Styles
  for ( int j = 0; j < layer.style.size(); j++ )
  {
    const QgsWmsStyleProperty &style = layer.style[j];

    metadata += "<tr><td>";
    metadata += tr( "Available in style" );
    metadata += "</td>";
    metadata += "<td>";

    // Nested table.
    metadata += "<table width=\"100%\">";

    // Layer Style Name
    metadata += "<tr><th class=\"glossy\">";
    metadata += tr( "Name" );
    metadata += "</th>";
    metadata += "<td>";
    metadata += style.name;
    metadata += "</td></tr>";

    // Layer Style Title
    metadata += "<tr><th class=\"glossy\">";
    metadata += tr( "Title" );
    metadata += "</th>";
    metadata += "<td>";
    metadata += style.title;
    metadata += "</td></tr>";

    // Layer Style Abstract
    metadata += "<tr><th class=\"glossy\">";
    metadata += tr( "Abstract" );
    metadata += "</th>";
    metadata += "<td>";
    metadata += style.abstract;
    metadata += "</td></tr>";

    // LegendURLs
    if ( !style.legendUrl.isEmpty() )
    {
      metadata += "<tr><th class=\"glossy\">";
      metadata += tr( "LegendURLs" );
      metadata += "</th>";
      metadata += "<td><table>";
      metadata += "<tr><th>Format</th><th>URL</th></tr>";
      for ( int k = 0; k < style.legendUrl.size(); k++ )
      {
        const QgsWmsLegendUrlProperty &l = style.legendUrl[k];
        metadata += "<tr><td>" + l.format + "</td><td>" + l.onlineResource.xlinkHref + "</td></tr>";
      }
      metadata += "</table></td></tr>";
    }

    // Close the nested table
    metadata += "</table>";
    metadata += "</td></tr>";
  }

  // Close the nested table
  metadata += "</table>";
  metadata += "</td></tr>";

  return metadata;
}

QString QgsWmsProvider::metadata()
{
  QString metadata = "";

  metadata += "<tr><td>";

  metadata += "<a href=\"#serverproperties\">";
  metadata += tr( "Server Properties" );
  metadata += "</a> ";

  metadata += "&nbsp;<a href=\"#selectedlayers\">";
  metadata += tr( "Selected Layers" );
  metadata += "</a>&nbsp;<a href=\"#otherlayers\">";
  metadata += tr( "Other Layers" );
  metadata += "</a>";

  if ( mTileLayersSupported.size() > 0 )
  {
    metadata += "<a href=\"#tilelayerproperties\">";
    metadata += tr( "Tile Layer Properties" );
    metadata += "</a> ";

    metadata += "<a href=\"#cachestats\">";
    metadata += tr( "Cache Stats" );
    metadata += "</a> ";
  }

  metadata += "</td></tr>";

  // Server Properties section
  metadata += "<tr><th class=\"glossy\"><a name=\"serverproperties\"></a>";
  metadata += tr( "Server Properties" );
  metadata += "</th></tr>";

  // Use a nested table
  metadata += "<tr><td>";
  metadata += "<table width=\"100%\">";

  // Table header
  metadata += "<tr><th class=\"glossy\">";
  metadata += tr( "Property" );
  metadata += "</th>";
  metadata += "<th class=\"glossy\">";
  metadata += tr( "Value" );
  metadata += "</th></tr>";

  // WMS Version
  metadata += "<tr><td>";
  metadata += tr( "WMS Version" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.version;
  metadata += "</td></tr>";

  // Service Title
  metadata += "<tr><td>";
  metadata += tr( "Title" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.title;
  metadata += "</td></tr>";

  // Service Abstract
  metadata += "<tr><td>";
  metadata += tr( "Abstract" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.abstract;
  metadata += "</td></tr>";

  // Service Keywords
  metadata += "<tr><td>";
  metadata += tr( "Keywords" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.keywordList.join( "<br />" );
  metadata += "</td></tr>";

  // Service Online Resource
  metadata += "<tr><td>";
  metadata += tr( "Online Resource" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += "-";
  metadata += "</td></tr>";

  // Service Contact Information
  metadata += "<tr><td>";
  metadata += tr( "Contact Person" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.contactInformation.contactPersonPrimary.contactPerson;
  metadata += "<br />";
  metadata += mCapabilities.service.contactInformation.contactPosition;
  metadata += "<br />";
  metadata += mCapabilities.service.contactInformation.contactPersonPrimary.contactOrganization;
  metadata += "</td></tr>";

  // Service Fees
  metadata += "<tr><td>";
  metadata += tr( "Fees" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.fees;
  metadata += "</td></tr>";

  // Service Access Constraints
  metadata += "<tr><td>";
  metadata += tr( "Access Constraints" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.service.accessConstraints;
  metadata += "</td></tr>";

  // GetMap Request Formats
  metadata += "<tr><td>";
  metadata += tr( "Image Formats" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.capability.request.getMap.format.join( "<br />" );
  metadata += "</td></tr>";

  // GetFeatureInfo Request Formats
  metadata += "<tr><td>";
  metadata += tr( "Identify Formats" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mCapabilities.capability.request.getFeatureInfo.format.join( "<br />" );
  metadata += "</td></tr>";

  // Layer Count (as managed by this provider)
  metadata += "<tr><td>";
  metadata += tr( "Layer Count" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += QString::number( mLayersSupported.size() );
  metadata += "</td></tr>";

  // Tileset Count (as managed by this provider)
  if ( mTileLayersSupported.size() > 0 )
  {
    metadata += "<tr><td>";
    metadata += tr( "Tile Layer Count" );
    metadata += "</td>";
    metadata += "<td>";
    metadata += QString::number( mTileLayersSupported.size() );
    metadata += "</td></tr>";
  }

  // Base URL
  metadata += "<tr><td>";
  metadata += tr( "GetCapabilitiesUrl" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += mBaseUrl;
  metadata += "</td></tr>";

  metadata += "<tr><td>";
  metadata += tr( "GetMapUrl" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += getMapUrl() + ( mIgnoreGetMapUrl ? tr( "&nbsp;<font color=\"red\">(advertised but ignored)</font>" ) : "" );
  metadata += "</td></tr>";

  metadata += "<tr><td>";
  metadata += tr( "GetFeatureInfoUrl" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += getFeatureInfoUrl() + ( mIgnoreGetFeatureInfoUrl ? tr( "&nbsp;<font color=\"red\">(advertised but ignored)</font>" ) : "" );
  metadata += "</td></tr>";

  metadata += "<tr><td>";
  metadata += tr( "GetLegendGraphic" );
  metadata += "</td>";
  metadata += "<td>";
  metadata += getLegendGraphicUrl() + ( mIgnoreGetMapUrl ? tr( "&nbsp;<font color=\"red\">(advertised but ignored)</font>" ) : "" );
  metadata += "</td></tr>";

  if ( mTiled )
  {
    metadata += "<tr><td>";
    metadata += tr( "GetTileUrl" );
    metadata += "</td>";
    metadata += "<td>";
    metadata += getTileUrl();
    metadata += "</td></tr>";

    if ( mTileLayer )
    {
      metadata += "<tr><td>";
      metadata += tr( "Tile templates" );
      metadata += "</td>";
      metadata += "<td>";
      for ( QHash<QString, QString>::const_iterator it = mTileLayer->getTileURLs.constBegin();
            it != mTileLayer->getTileURLs.constEnd();
            ++it )
      {
        metadata += QString( "%1:%2<br>" ).arg( it.key() ).arg( it.value() );
      }
      metadata += "</td></tr>";

      metadata += "<tr><td>";
      metadata += tr( "FeatureInfo templates" );
      metadata += "</td>";
      metadata += "<td>";
      for ( QHash<QString, QString>::const_iterator it = mTileLayer->getFeatureInfoURLs.constBegin();
            it != mTileLayer->getFeatureInfoURLs.constEnd();
            ++it )
      {
        metadata += QString( "%1:%2<br>" ).arg( it.key() ).arg( it.value() );
      }
      metadata += "</td></tr>";
    }
  }

  // Close the nested table
  metadata += "</table>";
  metadata += "</td></tr>";

  // Layer properties
  metadata += "<tr><th class=\"glossy\"><a name=\"selectedlayers\"></a>";
  metadata += tr( "Selected Layers" );
  metadata += "</th></tr>";

  for ( int i = 0; i < mLayersSupported.size(); i++ )
  {
    if ( !mTiled && mActiveSubLayers.contains( mLayersSupported[i].name ) )
    {
      metadata += layerMetadata( mLayersSupported[i] );
    }
  } // for each layer

  // Layer properties
  metadata += "<tr><th class=\"glossy\"><a name=\"otherlayers\"></a>";
  metadata += tr( "Other Layers" );
  metadata += "</th></tr>";

  for ( int i = 0; i < mLayersSupported.size(); i++ )
  {
    if ( !mActiveSubLayers.contains( mLayersSupported[i].name ) )
    {
      metadata += layerMetadata( mLayersSupported[i] );
    }
  } // for each layer

  // Tileset properties
  if ( mTileLayersSupported.size() > 0 )
  {
    metadata += "<tr><th class=\"glossy\"><a name=\"tilesetproperties\"></a>";
    metadata += tr( "Tileset Properties" );
    metadata += "</th></tr>";

    // Iterate through tilesets
    metadata += "<tr><td>";
    metadata += "<table width=\"100%\">";

    foreach ( const QgsWmtsTileLayer &l, mTileLayersSupported )
    {
      metadata += "<tr><td colspan=\"2\">";
      metadata += l.identifier;
      metadata += "</td><td class=\"glossy\">";

      if ( l.tileMode == WMTS )
      {
        metadata += tr( "WMTS" );
      }
      else if ( l.tileMode == WMSC )
      {
        metadata += tr( "WMS-C" );
      }
      else
      {
        Q_ASSERT( l.tileMode == WMTS || l.tileMode == WMSC );
      }

      metadata += "</td></tr>";

      // Table header
      metadata += "<tr><th class=\"glossy\">";
      metadata += tr( "Property" );
      metadata += "</th>";
      metadata += "<th class=\"glossy\">";
      metadata += tr( "Value" );
      metadata += "</th></tr>";

      metadata += "<tr><td class=\"glossy\">";
      metadata += tr( "Selected" );
      metadata += "</td>";
      metadata += "<td class=\"glossy\">";
      metadata += mTiled && l.identifier == mActiveSubLayers.join( "," ) ? tr( "Yes" ) : tr( "No" );
      metadata += "</td></tr>";

      if ( l.styles.size() > 0 )
      {
        metadata += "<tr><td class=\"glossy\">";
        metadata += tr( "Available Styles" );
        metadata += "</td>";
        metadata += "<td class=\"glossy\">";
        QStringList styles;
        foreach ( const QgsWmtsStyle &style, l.styles )
        {
          styles << style.identifier;
        }
        metadata += styles.join( ", " );
        metadata += "</td></tr>";
      }

      metadata += "<tr><td class=\"glossy\">";
      metadata += tr( "CRS" );
      metadata += "</td>";
      metadata += "<td class=\"glossy\">";
      metadata += l.boundingBox.crs;
      metadata += "</td></tr>";

      metadata += "<tr><td class=\"glossy\">";
      metadata += tr( "Bounding Box" );
      metadata += "</td>";
      metadata += "<td class=\"glossy\">";
      metadata += l.boundingBox.box.toString();
      metadata += "</td></tr>";

      metadata += "<tr><td class=\"glossy\">";
      metadata += tr( "Available Tilesets" );
      metadata += "</td><td class=\"glossy\">";

      foreach ( const QgsWmtsTileMatrixSetLink &setLink, l.setLinks )
      {
        metadata += setLink.tileMatrixSet + "<br>";
      }

      metadata += "</td></tr>";
    }

    metadata += "</table></td></tr>";

    if ( mTiled )
    {
      metadata += "<tr><th class=\"glossy\"><a name=\"cachestats\"></a>";
      metadata += tr( "Cache stats" );
      metadata += "</th></tr>";

      // Iterate through tilesets
      metadata += "<tr><td>";
      metadata += "<table width=\"100%\">";

      metadata += "<tr><th class=\"glossy\">";
      metadata += tr( "Property" );
      metadata += "</th>";
      metadata += "<th class=\"glossy\">";
      metadata += tr( "Value" );
      metadata += "</th></tr>";

      metadata += "<tr><td>";
      metadata += tr( "Hits" );
      metadata += "</td><td>";
      metadata += QString::number( mCacheHits );
      metadata += "</td></tr>";

      metadata += "<tr><td>";
      metadata += tr( "Misses" );
      metadata += "</td><td>";
      metadata += QString::number( mCacheMisses );
      metadata += "</td></tr>";

      metadata += "<tr><td>";
      metadata += tr( "Errors" );
      metadata += "</td><td>";
      metadata += QString::number( mErrors );
      metadata += "</td></tr>";

      metadata += "</table></td></tr>";
    }
  }

  metadata += "</table>";

  QgsDebugMsg( "exiting with '"  + metadata  + "'." );

  return metadata;
}

QgsRasterIdentifyResult QgsWmsProvider::identify( const QgsPoint & thePoint, QgsRaster::IdentifyFormat theFormat, const QgsRectangle &theExtent, int theWidth, int theHeight )
{
  QgsDebugMsg( QString( "theFormat = %1" ).arg( theFormat ) );
  QStringList resultStrings;
  QMap<int, QVariant> results;

  QString format;
  format = mIdentifyFormats.value( theFormat );
  if ( format.isEmpty() )
  {
    return QgsRasterIdentifyResult( ERROR( tr( "Format not supported" ) ) );
  }

  QgsDebugMsg( QString( "theFormat = %1 format = %2" ).arg( theFormat ).arg( format ) );

  if ( !extent().contains( thePoint ) )
  {
    results.insert( 1, "" );
    return QgsRasterIdentifyResult( theFormat, results );
  }

  QgsRectangle myExtent = theExtent;

  if ( !myExtent.isEmpty() )
  {
    // we cannot reliably identify WMS if theExtent is specified but theWidth or theHeight
    // are not, because we don't know original resolution
    if ( theWidth == 0 || theHeight == 0 )
    {
      return QgsRasterIdentifyResult( ERROR( tr( "Context not fully specified (extent was defined but width and/or height was not)." ) ) );
    }
  }
  else // context (theExtent, theWidth, theHeight) not defined
  {
    // We don't know original source resolution, so we take some small extent around the point.

    // Warning: this does not work well with poin/line vector layers where search rectangle
    // is based on pixel size (e.g. UMN Mapserver is using TOLERANCE layer param)

    double xRes = 0.001; // expecting meters

    // TODO: add CRS as class member
    QgsCoordinateReferenceSystem crs;
    if ( crs.createFromOgcWmsCrs( mImageCrs ) )
    {
      // set resolution approximately to 1mm
      switch ( crs.mapUnits() )
      {
        case QGis::Meters:
          xRes = 0.001;
          break;
        case QGis::Feet:
          xRes = 0.003;
          break;
        case QGis::Degrees:
          // max length of degree of latitude on pole is 111694 m
          xRes = 1e-8;
          break;
        default:
          xRes = 0.001; // expecting meters
      }
    }

    // Keep resolution in both axis equal! Otherwise silly server (like QGIS mapserver)
    // fail to calculate coordinate because it is using single resolution average!!!
    double yRes = xRes;

    // 1x1 should be sufficient but at least we know that GDAL ECW was very unefficient
    // so we use 2x2 (until we find that it is too small for some server)
    theWidth = theHeight = 2;

    myExtent = QgsRectangle( thePoint.x() - xRes, thePoint.y() - yRes,
                             thePoint.x() + xRes, thePoint.y() + yRes );
  }

  // Point in BBOX/WIDTH/HEIGHT coordinates
  // No need to fiddle with extent origin not covered by layer extent, I believe
  double xRes = myExtent.width() / theWidth;
  double yRes = myExtent.height() / theHeight;

  // Mapserver (6.0.3, for example) does not seem to work with 1x1 pixel box
  // (seems to be a different issue, not the slownes of GDAL with ECW mentioned above)
  // so we have to enlarge it a bit
  if ( theWidth == 1 )
  {
    theWidth += 1;
    myExtent.setXMaximum( myExtent.xMaximum() + xRes );
  }
  if ( theHeight == 1 )
  {
    theHeight += 1;
    myExtent.setYMaximum( myExtent.yMaximum() + yRes );
  }

  QgsDebugMsg( "myExtent = " + myExtent.toString() );
  QgsDebugMsg( QString( "theWidth = %1 theHeight = %2" ).arg( theWidth ).arg( theHeight ) );
  QgsDebugMsg( QString( "xRes = %1 yRes = %2" ).arg( xRes ).arg( yRes ) );

  QgsPoint point;
  point.setX( floor(( thePoint.x() - myExtent.xMinimum() ) / xRes ) );
  point.setY( floor(( myExtent.yMaximum() - thePoint.y() ) / yRes ) );

  QgsDebugMsg( QString( "point = %1 %2" ).arg( point.x() ).arg( point.y() ) );

  QgsDebugMsg( QString( "recalculated orig point (corner) = %1 %2" ).arg( myExtent.xMinimum() + point.x()*xRes ).arg( myExtent.yMaximum() - point.y()*yRes ) );

  // Collect which layers to query on
  //according to the WMS spec for 1.3, the order of x - and y - coordinates is inverted for geographical CRS
  bool changeXY = false;
  if ( !mIgnoreAxisOrientation && ( mCapabilities.version == "1.3.0" || mCapabilities.version == "1.3" ) )
  {
    //create CRS from string
    QgsCoordinateReferenceSystem theSrs;
    if ( theSrs.createFromOgcWmsCrs( mImageCrs ) && theSrs.axisInverted() )
    {
      changeXY = true;
    }
  }

  if ( mInvertAxisOrientation )
    changeXY = !changeXY;

  // compose the URL query string for the WMS server.
  QString crsKey = "SRS"; //SRS in 1.1.1 and CRS in 1.3.0
  if ( mCapabilities.version == "1.3.0" || mCapabilities.version == "1.3" )
  {
    crsKey = "CRS";
  }

  // Compose request to WMS server
  QString bbox = QString( changeXY ? "%2,%1,%4,%3" : "%1,%2,%3,%4" )
                 .arg( qgsDoubleToString( myExtent.xMinimum() ) )
                 .arg( qgsDoubleToString( myExtent.yMinimum() ) )
                 .arg( qgsDoubleToString( myExtent.xMaximum() ) )
                 .arg( qgsDoubleToString( myExtent.yMaximum() ) );

  //QgsFeatureList featureList;

  int count = -1;
  // Test for which layers are suitable for querying with
  for ( QStringList::const_iterator
        layers = mActiveSubLayers.begin(),
        styles = mActiveSubStyles.begin();
        layers != mActiveSubLayers.end();
        ++layers, ++styles )
  {
    count++;

    // Is sublayer visible?
    if ( !mActiveSubLayerVisibility.find( *layers ).value() )
    {
      // TODO: something better?
      // we need to keep all sublayers so that we can get their names in identify tool
      results.insert( count, false );
      continue;
    }

    // Is sublayer queryable?
    if ( !mQueryableForLayer.find( *layers ).value() )
    {
      results.insert( count, false );
      continue;
    }

    QgsDebugMsg( "Layer '" + *layers + "' is queryable." );

    QUrl requestUrl( mGetFeatureInfoUrlBase );
    setQueryItem( requestUrl, "SERVICE", "WMS" );
    setQueryItem( requestUrl, "VERSION", mCapabilities.version );
    setQueryItem( requestUrl, "REQUEST", "GetFeatureInfo" );
    setQueryItem( requestUrl, "BBOX", bbox );
    setQueryItem( requestUrl, crsKey, mImageCrs );
    setQueryItem( requestUrl, "WIDTH", QString::number( theWidth ) );
    setQueryItem( requestUrl, "HEIGHT", QString::number( theHeight ) );
    setQueryItem( requestUrl, "LAYERS", *layers );
    setQueryItem( requestUrl, "STYLES", *styles );
    setQueryItem( requestUrl, "FORMAT", mImageMimeType );
    setQueryItem( requestUrl, "QUERY_LAYERS", *layers );
    setQueryItem( requestUrl, "INFO_FORMAT", format );

    if ( mCapabilities.version == "1.3.0" || mCapabilities.version == "1.3" )
    {
      setQueryItem( requestUrl, "I", QString::number( point.x() ) );
      setQueryItem( requestUrl, "J", QString::number( point.y() ) );
    }
    else
    {
      setQueryItem( requestUrl, "X", QString::number( point.x() ) );
      setQueryItem( requestUrl, "Y", QString::number( point.y() ) );
    }

    if ( mFeatureCount > 0 )
    {
      setQueryItem( requestUrl, "FEATURE_COUNT", QString::number( mFeatureCount ) );
    }

    QgsDebugMsg( QString( "getfeatureinfo: %1" ).arg( requestUrl.toString() ) );
    QNetworkRequest request( requestUrl );
    setAuthorization( request );
    mIdentifyReply = QgsNetworkAccessManager::instance()->get( request );
    connect( mIdentifyReply, SIGNAL( finished() ), this, SLOT( identifyReplyFinished() ) );

    while ( mIdentifyReply )
    {
      QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents );
    }

    if ( mIdentifyResultBodies.size() == 0 ) // no result
    {
      QgsDebugMsg( "mIdentifyResultBodies is empty" );
      continue;
    }
    else if ( mIdentifyResultBodies.size() == 1 )
    {
      // Check for service exceptions (exceptions with ogr/gml are in the body)
      bool isXml = false;
      bool isGml = false;

      const QgsNetworkReplyParser::RawHeaderMap &headers = mIdentifyResultHeaders.value( 0 );
      foreach ( const QByteArray &v, headers.keys() )
      {
        if ( QString( v ).compare( "Content-Type", Qt::CaseInsensitive ) == 0 )
        {
          isXml = QString( headers.value( v ) ).compare( "text/xml", Qt::CaseInsensitive ) == 0;
          isGml = QString( headers.value( v ) ).compare( "ogr/gml", Qt::CaseInsensitive ) == 0;
          if ( isXml || isGml )
            break;
        }
      }

      if ( isGml || isXml )
      {
        QByteArray body = mIdentifyResultBodies.value( 0 );

        if ( isGml && body.startsWith( "Content-Type: text/xml\r\n\r\n" ) )
        {
          body = body.data() + strlen( "Content-Type: text/xml\r\n\r\n" );
          isXml = true;
        }

        if ( isXml && parseServiceExceptionReportDom( body ) )
        {
          QgsMessageLog::logMessage( tr( "Get feature info request error (Title:%1; Error:%2; URL: %3)" )
                                     .arg( mErrorCaption ).arg( mError )
                                     .arg( requestUrl.toString() ), tr( "WMS" ) );
          continue;
        }
      }
    }

    if ( theFormat == QgsRaster::IdentifyFormatHtml || theFormat == QgsRaster::IdentifyFormatText )
    {
      //resultStrings << mIdentifyResult;
      //results.insert( count, mIdentifyResult );
      results.insert( count, QString::fromUtf8( mIdentifyResultBodies.value( 0 ) ) );
    }
    else if ( theFormat == QgsRaster::IdentifyFormatFeature ) // GML
    {
      // The response maybe
      // 1) simple GML
      //    To get also geometry from UMN Mapserver, it must be enabled for layer, e.g.:
      //      LAYER
      //        METADATA
      //          "ows_geometries" "mygeom"
      //          "ows_mygeom_type" "polygon"
      //        END
      //      END

      // 2) multipart GML + XSD
      //    Multipart is supplied by UMN Mapserver following format is used
      //      OUTPUTFORMAT
      //        NAME "OGRGML"
      //        DRIVER "OGR/GML"
      //        FORMATOPTION "FORM=multipart"
      //      END
      //      WEB
      //        METADATA
      //          "wms_getfeatureinfo_formatlist" "OGRGML,text/html"
      //        END
      //      END
      //    GetFeatureInfo multipart response does not seem to be defined in
      //    OGC specification.


      int gmlPart = -1;
      int xsdPart = -1;
      for ( int i = 0; i < mIdentifyResultHeaders.size(); i++ )
      {
        if ( xsdPart == -1 && mIdentifyResultHeaders[i].value( "Content-Disposition" ).contains( ".xsd" ) )
        {
          xsdPart = i;
        }
        else if ( gmlPart == -1 && mIdentifyResultHeaders[i].value( "Content-Disposition" ).contains( ".dat" ) )
        {
          gmlPart = i;
        }

        if ( gmlPart != -1 && xsdPart != -1 )
          break;
      }

      if ( xsdPart == -1 && gmlPart == -1 )
      {
        if ( mIdentifyResultBodies.size() == 1 ) // GML
        {
          gmlPart = 0;
        }
        if ( mIdentifyResultBodies.size() == 2 ) // GML+XSD
        {
          QgsDebugMsg( "Multipart with 2 parts - expected GML + XSD" );
          // How to find which part is GML and which XSD? Both have
          // Content-Type: application/binary
          // different are Content-Disposition but it is not reliable.
          // We could analyze beginning of bodies...
          gmlPart = 0;
          xsdPart = 1;
        }
      }

      QByteArray gmlByteArray = mIdentifyResultBodies.value( gmlPart );
      QgsDebugMsg( "GML (first 2000 bytes):\n" + gmlByteArray.left( 2000 ) );

      // QgsGmlSchema.guessSchema() and QgsGml::getFeatures() are using Expat
      // which only accepts UTF-8, UTF-16, ISO-8859-1
      // http://sourceforge.net/p/expat/bugs/498/
      QDomDocument dom;
      dom.setContent( gmlByteArray ); // gets XML encoding
      QTextStream stream( &gmlByteArray );
      stream.setCodec( QTextCodec::codecForName( "UTF-8" ) );
      dom.save( stream, 4, QDomNode::EncodingFromTextStream );

      QGis::WkbType wkbType;
      QgsGmlSchema gmlSchema;

      if ( xsdPart >= 0 )  // XSD available
      {
#if 0
        // Validate GML by schema
        // Loading schema takes ages! It needs to load all XSD referenced in the schema,
        // for example:
        // http://schemas.opengis.net/gml/2.1.2/feature.xsd
        // http://schemas.opengis.net/gml/2.1.2/gml.xsd
        // http://schemas.opengis.net/gml/2.1.2/geometry.xsd
        // http://www.w3.org/1999/xlink.xsd
        // http://www.w3.org/2001/xml.xsd <- this takes 30s to download (2/2013)

        QXmlSchema schema;
        schema.load( mIdentifyResultBodies.value( xsdPart ) );
        // Unfortunately the schema cannot be successfully loaded, it reports error
        // "Element {http://www.opengis.net/gml}_Feature already defined"
        // there is probably a bug in QXmlSchema:
        // https://bugreports.qt-project.org/browse/QTBUG-8394
        // xmlpatternsvalidator gives the same error on XSD generated by OGR
        if ( !schema.isValid() )
        {
          // TODO: return QgsError
          results.insert( count, tr( "GML schema is not valid" ) );
          continue;
        }
        QXmlSchemaValidator validator( schema );
        if ( !validator.validate( mIdentifyResultBodies.value( gmlPart ) ) )
        {
          results.insert( count, tr( "GML is not valid" ) );
          continue;
        }
#endif
        QgsDebugMsg( "GML XSD (first 4000 bytes):\n" + QString::fromUtf8( mIdentifyResultBodies.value( xsdPart ).left( 4000 ) ) );
        gmlSchema.parseXSD( mIdentifyResultBodies.value( xsdPart ) );
      }
      else
      {
        // guess from GML
        bool ok = gmlSchema.guessSchema( gmlByteArray );
        if ( ! ok )
        {
          QgsError err = gmlSchema.error();
          err.append( tr( "Cannot identify" ) );
          QgsDebugMsg( "guess schema error: " +  err.message() );
          return QgsRasterIdentifyResult( err );
        }
      }

      QStringList featureTypeNames = gmlSchema.typeNames();
      QgsDebugMsg( QString( "%1 featureTypeNames found" ).arg( featureTypeNames.size() ) );

      // Each sublayer may have more features of different types, for example
      // if GROUP of multiple vector layers is used with UMN MapServer
      // Note: GROUP of layers in UMN MapServer is not queryable by default
      // (and I could not find a way to force it), it is possible however
      // to add another RASTER layer with the same name as group which is queryable
      // and has no DATA defined. Then such a layer may be add to QGIS and both
      // GetMap and GetFeatureInfo will return data for the group of the same name.
      // https://github.com/mapserver/mapserver/issues/318#issuecomment-4923208
      QgsFeatureStoreList featureStoreList;
      foreach ( QString featureTypeName, featureTypeNames )
      {
        QgsDebugMsg( QString( "featureTypeName = %1" ).arg( featureTypeName ) );

        QString geometryAttribute = gmlSchema.geometryAttributes( featureTypeName ).value( 0 );
        QList<QgsField> fieldList = gmlSchema.fields( featureTypeName );
        QgsDebugMsg( QString( "%1 fields found" ).arg( fieldList.size() ) );
        QgsFields fields;
        for ( int i = 0; i < fieldList.size(); i++ )
        {
          fields.append( fieldList[i] );
        }
        QgsGml gml( featureTypeName, geometryAttribute, fields );
        // TODO: avoid converting to string and back
        int ret = gml.getFeatures( gmlByteArray, &wkbType );
#ifdef QGISDEBUG
        QgsDebugMsg( QString( "parsing result = %1" ).arg( ret ) );
#else
        Q_UNUSED( ret );
#endif
        // TODO: all features coming from this layer should probably have the same CRS
        // the same as this layer, because layerExtentToOutputExtent() may be used
        // for results -> verify CRS and reprojects if necessary
        QMap<QgsFeatureId, QgsFeature* > features = gml.featuresMap();
        QgsDebugMsg( QString( "%1 features read" ).arg( features.size() ) );
        QgsFeatureStore featureStore( fields, crs() );
        QMap<QString, QVariant> params;
        params.insert( "sublayer", *layers );
        params.insert( "featureType", featureTypeName );
        params.insert( "getFeatureInfoUrl", requestUrl.toString() );
        featureStore.setParams( params );
        foreach ( QgsFeatureId id, features.keys() )
        {
          QgsFeature * feature = features.value( id );

          QgsDebugMsg( QString( "feature id = %1 : %2 attributes" ).arg( id ).arg( feature->attributes().size() ) );

          featureStore.features().append( QgsFeature( *feature ) );
        }
        featureStoreList.append( featureStore );
      }
      results.insert( count, qVariantFromValue( featureStoreList ) );
    }
  }

#if 0
  QString str;

  if ( theFormat == QgsRaster::IdentifyFormatHtml )
  {
    str = "<table>\n<tr><td>" + resultStrings.join( "</td></tr>\n<tr><td>" ) + "</td></tr>\n</table>";
    results.insert( 1, str );
  }
  else if ( theFormat == QgsRaster::IdentifyFormatText )
  {
    str = resultStrings.join( "\n-------------\n" );
    results.insert( 1, str );
  }
#endif

  return QgsRasterIdentifyResult( theFormat, results );
}

void QgsWmsProvider::identifyReplyFinished()
{
  mIdentifyResultHeaders.clear();
  mIdentifyResultBodies.clear();

  if ( mIdentifyReply->error() == QNetworkReply::NoError )
  {
    QVariant redirect = mIdentifyReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( !redirect.isNull() )
    {
      QgsDebugMsg( QString( "identify request redirected to %1" ).arg( redirect.toString() ) );
      emit statusChanged( tr( "identify request redirected." ) );

      mIdentifyReply->deleteLater();

      QgsDebugMsg( QString( "redirected getfeatureinfo: %1" ).arg( redirect.toString() ) );
      mIdentifyReply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( redirect.toUrl() ) );
      connect( mIdentifyReply, SIGNAL( finished() ), this, SLOT( identifyReplyFinished() ) );

      return;
    }

    QVariant status = mIdentifyReply->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    if ( !status.isNull() && status.toInt() >= 400 )
    {
      QVariant phrase = mIdentifyReply->attribute( QNetworkRequest::HttpReasonPhraseAttribute );
      mErrorFormat = "text/plain";
      mError = tr( "Map getfeatureinfo error %1: %2" ).arg( status.toInt() ).arg( phrase.toString() );
      emit statusChanged( mError );

      //mIdentifyResult = "";
    }

    QgsNetworkReplyParser parser( mIdentifyReply );
    if ( !parser.isValid() )
    {
      QgsDebugMsg( "Cannot parse reply" );
      mErrorFormat = "text/plain";
      mError = tr( "Cannot parse getfeatureinfo: %1" ).arg( parser.error() );
      emit statusChanged( mError );
      //mIdentifyResult = "";
    }
    else
    {
      // TODO: check headers, xsd ...
      QgsDebugMsg( QString( "%1 parts" ).arg( parser.parts() ) );
      mIdentifyResultBodies = parser.bodies();
      mIdentifyResultHeaders = parser.headers();
    }
  }
  else
  {
    //mIdentifyResult = tr( "ERROR: GetFeatureInfo failed" );
    mErrorFormat = "text/plain";
    mError = tr( "Map getfeatureinfo error: %1 [%2]" ).arg( mIdentifyReply->errorString() ).arg( mIdentifyReply->url().toString() );
    emit statusChanged( mError );
    QgsMessageLog::logMessage( mError, tr( "WMS" ) );
  }

  mIdentifyReply->deleteLater();
  mIdentifyReply = 0;
}


QgsCoordinateReferenceSystem QgsWmsProvider::crs()
{
  return mCrs;
}

QString QgsWmsProvider::lastErrorTitle()
{
  return mErrorCaption;
}


QString QgsWmsProvider::lastError()
{
  QgsDebugMsg( "returning '" + mError  + "'." );
  return mError;
}

QString QgsWmsProvider::lastErrorFormat()
{
  return mErrorFormat;
}

QString  QgsWmsProvider::name() const
{
  return WMS_KEY;
} //  QgsWmsProvider::name()


QString  QgsWmsProvider::description() const
{
  return WMS_DESCRIPTION;
} //  QgsWmsProvider::description()

void QgsWmsProvider::reloadData()
{
  delete mCachedImage;
  mCachedImage = 0;
}

void QgsWmsProvider::setAuthorization( QNetworkRequest &request ) const
{
  if ( !mUserName.isNull() || !mPassword.isNull() )
  {
    request.setRawHeader( "Authorization", "Basic " + QString( "%1:%2" ).arg( mUserName ).arg( mPassword ).toAscii().toBase64() );
  }

  if ( !mReferer.isNull() )
  {
    request.setRawHeader( "Referer", QString( "%1" ).arg( mReferer ).toAscii() );
  }
}

QVector<QgsWmsSupportedFormat> QgsWmsProvider::supportedFormats()
{
  QVector<QgsWmsSupportedFormat> formats;
  QStringList mFormats, mLabels;

  QList<QByteArray> supportedFormats = QImageReader::supportedImageFormats();

  if ( supportedFormats.contains( "png" ) )
  {
    QgsWmsSupportedFormat p1 = { "image/png", "PNG" };
    QgsWmsSupportedFormat p2 = { "image/png; mode=24bit", "PNG24" }; // UMN mapserver
    QgsWmsSupportedFormat p3 = { "image/png8", "PNG8" }; // used by geoserver
    QgsWmsSupportedFormat p4 = { "image/png; mode=8bit", "PNG8" }; // used by QGIS server and UMN mapserver
    QgsWmsSupportedFormat p5 = { "png", "PNG" }; // used by french IGN geoportail
    QgsWmsSupportedFormat p6 = { "pngt", "PNGT" }; // used by french IGN geoportail

    formats << p1 << p2 << p3 << p4 << p5 << p6;
  }

  if ( supportedFormats.contains( "jpg" ) )
  {
    QgsWmsSupportedFormat j1 = { "image/jpeg", "JPEG" };
    QgsWmsSupportedFormat j2 = { "jpeg", "JPEG" }; // used by french IGN geoportail
    formats << j1 << j2;
  }

  if ( supportedFormats.contains( "png" ) && supportedFormats.contains( "jpg" ) )
  {
    QgsWmsSupportedFormat g1 = { "image/x-jpegorpng", "JPEG/PNG" }; // used by cubewerx
    formats << g1;
  }

  if ( supportedFormats.contains( "gif" ) )
  {
    QgsWmsSupportedFormat g1 = { "image/gif", "GIF" };
    formats << g1;
  }

  if ( supportedFormats.contains( "tiff" ) )
  {
    QgsWmsSupportedFormat t1 = { "image/tiff", "TIFF" };
    formats << t1;
  }

  return formats;
}

QString QgsWmsProvider::nodeAttribute( const QDomElement &e, QString name, QString defValue )
{
  if ( e.hasAttribute( name ) )
    return e.attribute( name );

  QDomNamedNodeMap map( e.attributes() );
  for ( int i = 0; i < map.size(); i++ )
  {
    QDomAttr attr( map.item( i ).toElement().toAttr() );
    if ( attr.name().compare( name, Qt::CaseInsensitive ) == 0 )
      return attr.value();
  }

  return defValue;
}

void QgsWmsProvider::showMessageBox( const QString& title, const QString& text )
{
  QgsMessageOutput *message = QgsMessageOutput::createMessageOutput();
  message->setTitle( title );
  message->setMessage( text, QgsMessageOutput::MessageText );
  message->showMessage();
}

QImage QgsWmsProvider::getLegendGraphic( double scale, bool forceRefresh )
{
  // TODO manage return basing of getCapablity => avoid call if service is not available
  // some services dowsn't expose getLegendGraphic in capabilities but adding LegendURL in
  // the layer tags inside capabilities
  QgsDebugMsg( "entering." );

  QString lurl = getLegendGraphicUrl();

  if ( lurl.isEmpty() )
  {
    QgsDebugMsg( "getLegendGraphic url is empty" );
    return QImage();
  }

  forceRefresh |= mGetLegendGraphicImage.isNull() || mGetLegendGraphicScale != scale;
  if ( !forceRefresh )
    return mGetLegendGraphicImage;

  QUrl url( lurl );

  if ( !url.hasQueryItem( "SERVICE" ) )
    setQueryItem( url, "SERVICE", "WMS" );
  if ( !url.hasQueryItem( "VERSION" ) )
    setQueryItem( url, "VERSION", mCapabilities.version );
  if ( !url.hasQueryItem( "SLD_VERSION" ) )
    setQueryItem( url, "SLD_VERSION", "1.1.0" ); // can not determine SLD_VERSION
  if ( !url.hasQueryItem( "REQUEST" ) )
    setQueryItem( url, "REQUEST", "GetLegendGraphic" );
  if ( !url.hasQueryItem( "FORMAT" ) )
    setQueryItem( url, "FORMAT", mImageMimeType );
  if ( !url.hasQueryItem( "LAYER" ) )
    setQueryItem( url, "LAYER", mActiveSubLayers[0] );
  if ( !url.hasQueryItem( "STYLE" ) )
    setQueryItem( url, "STYLE", mActiveSubStyles[0] );

  // add config parameter related to resolution
  QSettings s;
  int defaultLegendGraphicResolution = s.value( "/qgis/defaultLegendGraphicResolution", 0 ).toInt();
  QgsDebugMsg( QString( "defaultLegendGraphicResolution: %1" ).arg( defaultLegendGraphicResolution ) );
  if ( defaultLegendGraphicResolution )
  {
    if ( mDpiMode & dpiQGIS )
      setQueryItem( url, "DPI", QString::number( defaultLegendGraphicResolution ) );
    if ( mDpiMode & dpiUMN )
    {
      setQueryItem( url, "MAP_RESOLUTION", QString::number( defaultLegendGraphicResolution ) );
      setQueryItem( url, "SCALE", QString::number( scale, 'f' ) );
    }
    if ( mDpiMode & dpiGeoServer )
    {
      setQueryItem( url, "FORMAT_OPTIONS", QString( "dpi:%1" ).arg( defaultLegendGraphicResolution ) );
      setQueryItem( url, "SCALE", QString::number( scale, 'f' ) );
    }
  }

  mGetLegendGraphicScale = scale;

  mError = "";

  QNetworkRequest request( url );
  setAuthorization( request );
  request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork );
  request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );

  QgsDebugMsg( QString( "getlegendgraphics: %1" ).arg( url.toString() ) );
  mGetLegendGraphicReply = QgsNetworkAccessManager::instance()->get( request );

  connect( mGetLegendGraphicReply, SIGNAL( finished() ), this, SLOT( getLegendGraphicReplyFinished() ) );
  connect( mGetLegendGraphicReply, SIGNAL( downloadProgress( qint64, qint64 ) ), this, SLOT( getLegendGraphicReplyProgress( qint64, qint64 ) ) );

  while ( mGetLegendGraphicReply )
  {
    QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents, WMS_THRESHOLD );
  }

  QgsDebugMsg( "exiting." );

  return mGetLegendGraphicImage;
}

void QgsWmsProvider::getLegendGraphicReplyFinished()
{
  QgsDebugMsg( "entering." );

  if ( mGetLegendGraphicReply->error() == QNetworkReply::NoError )
  {
    QgsDebugMsg( "reply ok" );
    QVariant redirect = mGetLegendGraphicReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    if ( !redirect.isNull() )
    {
      emit statusChanged( tr( "GetLegendGraphic request redirected." ) );

      const QUrl& toUrl = redirect.toUrl();
      mGetLegendGraphicReply->request();
      if ( toUrl == mGetLegendGraphicReply->url() )
      {
        mErrorFormat = "text/plain";
        mError = tr( "Redirect loop detected: %1" ).arg( toUrl.toString() );
        QgsMessageLog::logMessage( mError, tr( "WMS" ) );
        mHttpGetLegendGraphicResponse.clear();
      }
      else
      {
        QNetworkRequest request( toUrl );
        setAuthorization( request );
        request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork );
        request.setAttribute( QNetworkRequest::CacheSaveControlAttribute, true );

        mGetLegendGraphicReply->deleteLater();
        QgsDebugMsg( QString( "redirected GetLegendGraphic: %1" ).arg( redirect.toString() ) );
        mGetLegendGraphicReply = QgsNetworkAccessManager::instance()->get( request );

        connect( mGetLegendGraphicReply, SIGNAL( finished() ), this, SLOT( getLegendGraphicReplyFinished() ) );
        connect( mGetLegendGraphicReply, SIGNAL( downloadProgress( qint64, qint64 ) ), this, SLOT( getLegendGraphicReplyProgress( qint64, qint64 ) ) );
        return;
      }
    }

    QVariant status = mGetLegendGraphicReply->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    if ( !status.isNull() && status.toInt() >= 400 )
    {
      QVariant phrase = mGetLegendGraphicReply->attribute( QNetworkRequest::HttpReasonPhraseAttribute );
      showMessageBox( tr( "GetLegendGraphic request error" ), tr( "Status: %1\nReason phrase: %2" ).arg( status.toInt() ).arg( phrase.toString() ) );
    }
    else
    {
      QImage myLocalImage = QImage::fromData( mGetLegendGraphicReply->readAll() ) ;
      if ( myLocalImage.isNull() )
      {
        QgsMessageLog::logMessage( tr( "Returned legend image is flawed [URL: %1]" )
                                   .arg( mGetLegendGraphicReply->url().toString() ), tr( "WMS" ) );
      }
      else
      {
        mGetLegendGraphicImage = myLocalImage;

#ifdef QGISDEBUG
        QString filename = QDir::tempPath() + "/GetLegendGraphic.png";
        mGetLegendGraphicImage.save( filename );
        QgsDebugMsg( "saved GetLegendGraphic result in debug ile: " + filename );
#endif
      }
    }
  }
  else
  {
    QgsMessageLog::logMessage( tr( "Download of GetLegendGraphic failed: %1" ).arg( mGetLegendGraphicReply->errorString() ), tr( "WMS" ) );
    mHttpGetLegendGraphicResponse.clear();
  }

  mGetLegendGraphicReply->deleteLater();
  mGetLegendGraphicReply = 0;
}

void QgsWmsProvider::getLegendGraphicReplyProgress( qint64 bytesReceived, qint64 bytesTotal )
{
  QString msg = tr( "%1 of %2 bytes of GetLegendGraphic downloaded." ).arg( bytesReceived ).arg( bytesTotal < 0 ? QString( "unknown number of" ) : QString::number( bytesTotal ) );
  QgsDebugMsg( msg );
  emit statusChanged( msg );
}

/**
 * Class factory to return a pointer to a newly created
 * QgsWmsProvider object
 */
QGISEXTERN QgsWmsProvider * classFactory( const QString *uri )
{
  return new QgsWmsProvider( *uri );
}
/** Required key function (used to map the plugin to a data store type)
*/
QGISEXTERN QString providerKey()
{
  return WMS_KEY;
}
/**
 * Required description function
 */
QGISEXTERN QString description()
{
  return WMS_DESCRIPTION;
}
/**
 * Required isProvider function. Used to determine if this shared library
 * is a data provider plugin
 */
QGISEXTERN bool isProvider()
{
  return true;
}
