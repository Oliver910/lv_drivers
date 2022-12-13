
#include "vncserver.h"
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/select.h>

/* According to earlier standards */
#include <sys/time.h>

#include <ifaddrs.h>
#include <net/if.h>

/*********************
 *      DEFINES

    *********************/
typedef enum enSTATE {
  enState_Protocol = 0,
  enState_Init,
  enState_Connected,
  enState_MAXBuff,
} enSTATE;

typedef enum enClientMsg {
  SetPixelFormat = 0,
  FixColourMapEntries = 1,
  SetEncodings = 2,
  FramebufferUpdateRequest = 3,
  KeyEvent = 4,
  PointerEvent = 5,
  ClientCutText = 6
} enClientMsg;

typedef enum enServerMsg {
  FramebufferUpdate = 0,
  SetColourMapEntries = 1
} enServerMsg;

typedef enum enButtonAction {
  NoButton,
  LeftButton,
  MidButton,
  RightButton
} enButtonAction;

typedef enum enWheelDirection {
  WheelNone,
  WheelUp,
  WheelDown,
  WheelLeft,
  WheelRight
} enWheelDirection;

typedef struct RfbRect {
  uint16_t x, y, h, w;
} RfbRect;

typedef struct vnc_context {
  int serverFd;
  int clientFd;
  /// @brief for vnc state manage
  uint8_t state;

  int running;
  int handleMsg;
  uint8_t msgType;
  uint8_t wantUpdate;
  uint8_t dirtyCursor;
  /// @brief for pixel format
  uint8_t sameEndian;
  uint8_t needConversion;
  uint32_t swapBytes;
  /// @brief for encoder
  uint16_t count;
  int encodingsPending;

  uint supportCopyRect : 1;
  uint supportRRE : 1;
  uint supportCoRRE : 1;
  uint supportHextile : 1;
  uint supportZRLE : 1;
  uint supportCursor : 1;
  uint supportDesktopSize : 1;
  ///
  void *encoder;

  /// @brief update frame buffer
  uint8_t incremental;
  RfbRect rct;

  /// @brief wheel
  int mouse_act;
  int mouse_posx;
  int mouse_posy;
  enButtonAction buttons;
  enWheelDirection wheelDirection;

  /// @brief for key
  uint8_t key_down;
  uint32_t unicode;
  uint32_t keycode;

  /// @brief for socket data recv data.
  char buf[256];
  int buf_len;
} vnc_context;

typedef struct RfbPixelFormat {
  // public:
  //     static int size() { return 16; }
  //     void read(QTcpSocket *s);
  //     void write(QTcpSocket *s);
  int bitsPerPixel;
  int depth;
  bool bigEndian;
  bool trueColor;
  int redBits;
  int greenBits;
  int blueBits;
  int redShift;
  int greenShift;
  int blueShift;
} RfbPixelFormat;

static const char *cs_server_name = "oliverVNC";

/**********************
 *  STATIC PROTOTYPES
 **********************/
void *nvcserver_mainloop(void *pParm);

/**********************
 *  STATIC VARIABLES
 **********************/
static int s_fdvnc = -1;
static int s_port = 5900;
static pthread_t s_phdLoop = 0;
static int s_vnc_running = 0;
static RfbPixelFormat s_pixformat = {.bitsPerPixel = 32,
                                     .depth = 32,
                                     .bigEndian = 0,
                                     .trueColor = true,
                                     .redBits = 8,
                                     .greenBits = 8,
                                     .blueBits = 8,
                                     .redShift = 16,
                                     .greenShift = 8,
                                     .blueShift = 0};

static lv_indev_data_t s_indev_data;
static int s_mouse_act = 0;
/*********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void vncserver_init(void) {
  int ret;
  s_vnc_running = 1;
  ret = pthread_create(&s_phdLoop, NULL, nvcserver_mainloop, NULL);
}
void vncserver_exit(void) {}

/**
 * Flush a buffer to the marked area
 * @param drv pointer to driver where this function belongs
 * @param area an area where to copy `color_p`
 * @param color_p an array of pixel to copy to the `area` part of the screen
 */
void vncserver_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p) {}

void vncserver_get_sizes(uint32_t *width, uint32_t *height) {
  //  if (width)
  //      *width = vinfo.xres;
  //
  //   if (height)
  //      *height = vinfo.yres;
}

int vnc_mouse_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  (void)indev_drv; /*Unused*/

  /*Store the collected data*/
  // data->point.x = 100;
  // data->point.y = 100;
  // data->state = LV_INDEV_STATE_RELEASED;
  //     left_button_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

  if (s_mouse_act == 1) {
    data->point.x = s_indev_data.point.x;
    data->point.y = s_indev_data.point.y;
    data->state = s_indev_data.state;
    s_mouse_act = 0;
    printf("vnc_mouse_read [%d,%d]\n", data->point.x, data->point.y);
    return 1;
  }
  return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static int init_vncserver(void) {
  int sockfd = -1;
  const int enable = 1;
  sockfd = socket(AF_INET, SOCK_STREAM, 0);

  if (sockfd == -1) {
    printf("Fail to create a socket.");
    return -1;
  }
  // socket的連線
  struct sockaddr_in serverInfo, clientInfo;
  int addrlen = sizeof(clientInfo);
  bzero(&serverInfo, sizeof(serverInfo));

  serverInfo.sin_family = PF_INET;
  serverInfo.sin_addr.s_addr = INADDR_ANY;
  serverInfo.sin_port = htons(s_port);
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    perror("setsockopt(SO_REUSEADDR) failed");
  bind(sockfd, (struct sockaddr *)&serverInfo, sizeof(serverInfo));

  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    perror("setsockopt(SO_REUSEADDR) failed");

  listen(sockfd, 5);
  return sockfd;
}

void ctx_dequeue(vnc_context *pCtx, uint8_t *dest, int nLen) {
  memcpy(dest, pCtx->buf, nLen);
  pCtx->buf_len = pCtx->buf_len - nLen;
  memmove(pCtx->buf, &pCtx->buf[nLen], pCtx->buf_len);
}

void state_protocol(vnc_context *pCtx) {
  if (pCtx->buf_len >= 12) {
    char proto[13];
    ctx_dequeue(pCtx, proto, 12);
    proto[12] = '\0';
    printf("Client protocol version %s", proto);
    // No authentication
    uint32_t auth = htonl(1);
    send(pCtx->clientFd, (char *)&auth, sizeof(auth), 0);
    pCtx->state = enState_Init;
  }
}
void write_Rfb_pixel_format(vnc_context *pCtx) {
  char buf[16];
  buf[0] = s_pixformat.bitsPerPixel;
  buf[1] = s_pixformat.depth;
  buf[2] = s_pixformat.bigEndian;
  buf[3] = s_pixformat.trueColor;
  uint16_t a = 0;
  for (int i = 0; i < s_pixformat.redBits; i++)
    a = (a << 1) | 1;
  *(uint16_t *)(buf + 4) = htons(a);
  a = 0;
  for (int i = 0; i < s_pixformat.greenBits; i++)
    a = (a << 1) | 1;
  *(uint16_t *)(buf + 6) = htons(a);
  a = 0;
  for (int i = 0; i < s_pixformat.blueBits; i++)
    a = (a << 1) | 1;
  *(uint16_t *)(buf + 8) = htons(a);
  buf[10] = s_pixformat.redShift;
  buf[11] = s_pixformat.greenShift;
  buf[12] = s_pixformat.blueShift;
  send(pCtx->clientFd, buf, 16, 0);
}

void read_Rfb_pixel_format(vnc_context *pCtx) {
  char buf[16];
  ctx_dequeue(pCtx, buf, 16);
  s_pixformat.bitsPerPixel = buf[0];
  s_pixformat.depth = buf[1];
  s_pixformat.bigEndian = buf[2];
  s_pixformat.trueColor = buf[3];
  uint16_t a = ntohs(*(uint16_t *)(buf + 4));
  s_pixformat.redBits = 0;
  while (a) {
    a >>= 1;
    s_pixformat.redBits++;
  }
  a = ntohs(*(uint16_t *)(buf + 6));
  s_pixformat.greenBits = 0;
  while (a) {
    a >>= 1;
    s_pixformat.greenBits++;
  }
  a = ntohs(*(uint16_t *)(buf + 8));
  s_pixformat.blueBits = 0;
  while (a) {
    a >>= 1;
    s_pixformat.blueBits++;
  }
  s_pixformat.redShift = buf[10];
  s_pixformat.greenShift = buf[11];
  s_pixformat.blueShift = buf[12];
}

void state_init(vnc_context *pCtx) {
  if (pCtx->buf_len >= 1) {
    char shared;
    uint32_t len = strlen(cs_server_name);
    ctx_dequeue(pCtx, &shared, 1);
    printf("shared = %x\n", shared);

    uint16_t width = htons(800);
    uint16_t height = htons(600);
    send(pCtx->clientFd, (char *)&width, sizeof(width), 0);
    send(pCtx->clientFd, (char *)&height, sizeof(height), 0);
    write_Rfb_pixel_format(pCtx);

    len = htonl(len);
    send(pCtx->clientFd, &len, 4, 0);
    send(pCtx->clientFd, cs_server_name, strlen(cs_server_name), 0);
    pCtx->state = enState_Connected;
  }
}

static void setPixelFormat(vnc_context *pCtx) {
  if (pCtx->buf_len >= 19) {
    char buf[3];
    ctx_dequeue(pCtx, buf, 3);
    read_Rfb_pixel_format(pCtx);

    printf("Want format: %d %d %d %d %d %d %d %d %d %d",
           s_pixformat.bitsPerPixel, s_pixformat.depth, s_pixformat.bigEndian,
           s_pixformat.trueColor, s_pixformat.redBits, s_pixformat.greenBits,
           s_pixformat.blueBits, s_pixformat.redShift, s_pixformat.greenShift,
           s_pixformat.blueShift);

    if (!s_pixformat.trueColor) {
      printf("Can only handle true color clients");
      // discardClient();
      exit(0);
    }
    pCtx->handleMsg = false;
    pCtx->sameEndian = true;
    pCtx->needConversion = 0;
    pCtx->swapBytes = 0;
#if 0
    sameEndian =
        (QSysInfo::ByteOrder == QSysInfo::BigEndian) == !!pixelFormat.bigEndian;
    needConversion = pixelConversionNeeded();
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    swapBytes = qvnc_screen->swapBytes();
#endif
#endif
  }
}

bool read_QRfbSetEncodings(vnc_context *pCtx) {
  if (pCtx->buf_len < 3)
    return false;

  char tmp;
  ctx_dequeue(pCtx, &tmp, 1); // padding
  ctx_dequeue(pCtx, (char *)&pCtx->count, 2);
  pCtx->count = ntohs(pCtx->count);
  return true;
}

void set_encodings(vnc_context *pCtx) {

  // QRfbSetEncodings enc;

  if (!pCtx->encodingsPending && read_QRfbSetEncodings(pCtx)) {
    pCtx->encodingsPending = pCtx->count;
    if (!pCtx->encodingsPending)
      pCtx->handleMsg = false;
  }

  if (pCtx->encoder) {
    // delete encoder;
    pCtx->encoder = NULL;
  }

  enum Encodings {
    Raw = 0,
    CopyRect = 1,
    RRE = 2,
    CoRRE = 4,
    Hextile = 5,
    ZRLE = 16,
    Cursor = -239,
    DesktopSize = -223
  };

  pCtx->supportCursor = false;

  if (pCtx->encodingsPending &&
      (unsigned)pCtx->buf_len >= pCtx->encodingsPending * sizeof(uint32_t)) {
    for (int i = 0; i < pCtx->encodingsPending; ++i) {
      int32_t enc;
      ctx_dequeue(pCtx, &enc, sizeof(int32_t));
      enc = ntohl(enc);
#if 1
      printf("QVNCServer::setEncodings: %d\n", enc);
#endif
      switch (enc) {
      case Raw:
        if (!pCtx->encoder) {
          // encoder = new QRfbRawEncoder(this);
#if 1
          printf("QVNCServer::setEncodings: using raw");
#endif
        }
        break;
      case CopyRect:
        pCtx->supportCopyRect = true;
        break;
      case RRE:
        pCtx->supportRRE = true;
        break;
      case CoRRE:
        pCtx->supportCoRRE = true;
        break;
      case Hextile:
        pCtx->supportHextile = true;
        if (pCtx->encoder)
          break;
//         switch (qvnc_screen->depth()) {
// #ifdef QT_QWS_DEPTH_8
//         case 8:
//           encoder = new QRfbHextileEncoder<quint8>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_12
//         case 12:
//           encoder = new QRfbHextileEncoder<qrgb444>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_15
//         case 15:
//           encoder = new QRfbHextileEncoder<qrgb555>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_16
//         case 16:
//           encoder = new QRfbHextileEncoder<quint16>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_18
//         case 18:
//           encoder = new QRfbHextileEncoder<qrgb666>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_24
//         case 24:
//           encoder = new QRfbHextileEncoder<qrgb888>(this);
//           break;
// #endif
// #ifdef QT_QWS_DEPTH_32
//         case 32:
//           encoder = new QRfbHextileEncoder<quint32>(this);
//           break;
// #endif
//         default:
//           break;
//         }
#if 1
        printf("QVNCServer::setEncodings: using hextile");
#endif
        break;
      case ZRLE:
        pCtx->supportZRLE = true;
        break;
      case Cursor:
        pCtx->supportCursor = true;
        // #ifndef QT_NO_QWS_CURSOR
        //         if (!qvnc_screen->screen() ||
        //         qt_screencursor->isAccelerated()) {
        //           delete qvnc_cursor;
        //           qvnc_cursor = new QVNCClientCursor(this);
        //         }
        // #endif
        break;
      case DesktopSize:
        pCtx->supportDesktopSize = true;
        break;
      default:
        break;
      }
    }
    pCtx->handleMsg = false;
    pCtx->encodingsPending = 0;
  }

  if (!pCtx->encoder) {
    // pCtx->encoder = new QRfbRawEncoder(this);
#if 1
    printf("QVNCServer::setEncodings: fallback using raw");
#endif
  }

  // if (cursor)
  //   cursor->setCursorMode(supportCursor);
}

void read_QRfbRect(vnc_context *pCtx) {
  uint16_t buf[4];
  ctx_dequeue(pCtx, (char *)buf, 8);
  pCtx->rct.x = ntohs(buf[0]);
  pCtx->rct.y = ntohs(buf[1]);
  pCtx->rct.w = ntohs(buf[2]);
  pCtx->rct.h = ntohs(buf[3]);
}

void write_QRfbRect(vnc_context *pCtx) {
  int16_t buf[4];
  buf[0] = htons(pCtx->rct.x);
  buf[1] = htons(pCtx->rct.y);
  buf[2] = htons(pCtx->rct.w);
  buf[3] = htons(pCtx->rct.h);
  send(pCtx->clientFd, (char *)buf, 8, 0);
}

bool read_QRfbFrameBufferUpdateRequest(vnc_context *pCtx) {
  if (pCtx->buf_len < 9)
    return false;

  ctx_dequeue(pCtx, &pCtx->incremental, 1);
  read_QRfbRect(pCtx);
  return true;
}
extern uint32_t pixelbuf[];

void write_QRfbRawEncoder(vnc_context *pCtx) {
  const char tmp[2] = {0, 0}; // msg type, padding
  send(pCtx->clientFd, tmp, sizeof(tmp), 0);
  const uint16_t count = htons(1);
  send(pCtx->clientFd, (char *)&count, sizeof(count), 0);

  if (count <= 0) {
    //        QWSDisplay::ungrab();
    return;
  }

  // for qrect
  pCtx->rct.x = 0;
  pCtx->rct.y = 0;
  pCtx->rct.w = 800;
  pCtx->rct.h = 600;
  write_QRfbRect(pCtx);
  const uint32_t encoding = htonl(0); // raw encoding
  send(pCtx->clientFd, (char *)&encoding, sizeof(encoding), 0);

  int32_t fbptr[800 * 600];
  get_monitor_param(fbptr);
  // FILE *fp = fopen("/home/oliver/4.ram", "wb+");
  // if (fp) {
  //   int ret = fwrite(fbptr, 4, 800 * 600, fp);
  //   printf("ret = %d %p\n", ret, fbptr);
  //   fflush(fp);
  //   fclose(fp);
  // }
  int8_t *ppfb32 = (int8_t *)pixelbuf;
  for (int i = 0; i < pCtx->rct.h; ++i) {
    send(pCtx->clientFd, (const char *)ppfb32, pCtx->rct.w * 4, 0);
    ppfb32 += (pCtx->rct.w * 4);
  }

  // const QImage *screenImage = server->screenImage();

  // for (int i = 0; i < rects.size(); ++i) {
  //   const QRect tileRect = rects.at(i);
  //   const QRfbRect rect(tileRect.x(), tileRect.y(), tileRect.width(),
  //                      tileRect.height());
  // rect.write(socket);

  // const quint32 encoding = htonl(0); // raw encoding
  // socket->write((char *)&encoding, sizeof(encoding));

  // int linestep = screenImage->bytesPerLine();
  // const uchar *screendata =
  //     screenImage->scanLine(rect.y) + rect.x * screenImage->depth() / 8;

  // #ifndef QT_NO_QWS_CURSOR
  //     // hardware cursors must be blended with the
  //     screen memory const bool doBlendCursor =
  //     qt_screencursor && !server->hasClientCursor() &&
  //                                qt_screencursor->isAccelerated();
  //     QImage tileImage;
  //     if (doBlendCursor) {
  //       const QRect cursorRect =
  //       qt_screencursor->boundingRect().translated(
  //           -server->screen()->offset());
  //       if (tileRect.intersects(cursorRect)) {
  //         tileImage = screenImage->copy(tileRect);
  //         blendCursor(tileImage,
  //         tileRect.translated(server->screen()->offset()));
  //         screendata = tileImage.bits(); linestep =
  //         tileImage.bytesPerLine();
  //       }
  //     }
  // #endif // QT_NO_QWS_CURSOR

  // if (server->doPixelConversion()) {
  //   const int bufferSize = rect.w * rect.h *
  //   bytesPerPixel; if (bufferSize > buffer.size())
  //     buffer.resize(bufferSize);

  //   // convert pixels
  //   char *b = buffer.data();
  //   const int bstep = rect.w * bytesPerPixel;
  //   for (int i = 0; i < rect.h; ++i) {
  //     server->convertPixels(b, (const char
  //     *)screendata, rect.w); screendata += linestep; b
  //     += bstep;
  //   }
  //   socket->write(buffer.constData(), bufferSize);
  // } else

  // {
  //   for (int i = 0; i < rect.h; ++i) {
  //     socket->write((const char *)screendata, rect.w * bytesPerPixel);
  //     screendata += linestep;
  //   }
  // }
  // if (socket->state() ==
  // QAbstractSocket::UnconnectedState)
  //   break;
  //}
  fsync(pCtx->clientFd);

  //    QWSDisplay::ungrab();
}

void checkUpdate(vnc_context *pCtx) {
  if (!pCtx->wantUpdate)
    return;

  if (pCtx->dirtyCursor) {
    // #ifndef QT_NO_QWS_CURSOR
    //     Q_ASSERT(qvnc_cursor);
    //     qvnc_cursor->write();
    // #endif
    //     cursor->sendClientCursor();
    pCtx->dirtyCursor = false;
    pCtx->wantUpdate = false;
    return;
  }
  write_QRfbRawEncoder(pCtx);
  pCtx->wantUpdate = false;
  // if (dirtyMap()->numDirty > 0) {
  //   if (pCtx->encoder)
  //     encoder->write();
  //   pCtx->wantUpdate = false;
  // }
}

void frameBuffer_update_request(vnc_context *pCtx) {
  // QRfbFrameBufferUpdateRequest ev;

  if (read_QRfbFrameBufferUpdateRequest(pCtx)) {
    if (!pCtx->incremental) {
      printf("screen[%d %d %d %d]\n", pCtx->rct.x, pCtx->rct.y, pCtx->rct.w,
             pCtx->rct.h);
      // QRect r(ev.rect.x, ev.rect.y, ev.rect.w,
      // ev.rect.h);
      // ////### r.translate(qvnc_screen->offset());
      // qvnc_screen->d_ptr->setDirty(r, true);
    }
    pCtx->wantUpdate = true;
    checkUpdate(pCtx);
    pCtx->handleMsg = false;
  }
}

bool read_QRfbPointerEvent(vnc_context *pCtx) {
  if (pCtx->buf_len < 5)
    return false;

  char buttonMask;
  ctx_dequeue(pCtx, &buttonMask, 1);

  pCtx->buttons = NoButton;
  pCtx->wheelDirection = WheelNone;
  if (buttonMask & 1)
    pCtx->buttons |= LeftButton;
  if (buttonMask & 2)
    pCtx->buttons |= MidButton;
  if (buttonMask & 4)
    pCtx->buttons |= RightButton;
  if (buttonMask & 8)
    pCtx->wheelDirection = WheelUp;
  if (buttonMask & 16)
    pCtx->wheelDirection = WheelDown;
  if (buttonMask & 32)
    pCtx->wheelDirection = WheelLeft;
  if (buttonMask & 64)
    pCtx->wheelDirection = WheelRight;

  uint16_t tmp;
  ctx_dequeue(pCtx, (char *)&tmp, 2);
  pCtx->mouse_posx = ntohs(tmp);
  ctx_dequeue(pCtx, (char *)&tmp, 2);
  pCtx->mouse_posy = ntohs(tmp);

  return true;
}

void pointerEvent(vnc_context *pCtx) {
  // QPoint screenOffset =
  // this->screen()->geometry().topLeft();

  // QRfbPointerEvent ev;
  printf("pointerEvent[%d,%d]\n", pCtx->mouse_posx, pCtx->mouse_posy);
  if (read_QRfbPointerEvent(pCtx)) {
    printf("pos[%d,%d]\n", pCtx->mouse_posx, pCtx->mouse_posy);
    if (pCtx->wheelDirection == WheelNone) {
      // QEvent::Type type = QEvent::MouseMove;
      // Qt::MouseButton button = Qt::NoButton;
      // bool isPress;
      // if (buttonChange(buttons, ev.buttons, &button,
      // &isPress))
      //   type = isPress ? QEvent::MouseButtonPress :
      //   QEvent::MouseButtonRelease;
      // QWindowSystemInterface::handleMouseEvent(0,
      // eventPoint, eventPoint,
      //                                          ev.buttons);
    } else {
      // No buttons or motion reported at the same time as
      // wheel events Qt::Orientation orientation; if
      // (pCtx->wheelDirection == WheelLeft ||
      //     pCtx->wheelDirection == WheelRight)
      //   orientation = Qt::Horizontal;
      // else
      //   orientation = Qt::Vertical;

      int delta = 120 * ((pCtx->wheelDirection == WheelLeft ||
                          pCtx->wheelDirection == WheelUp)
                             ? 1
                             : -1);
      // QWindowSystemInterface::handleWheelEvent(0,
      // eventPoint, eventPoint, delta,
      //                                          orientation);
    }
    s_indev_data.point.x = pCtx->mouse_posx;
    s_indev_data.point.y = pCtx->mouse_posy;
    s_indev_data.state = pCtx->buttons == NoButton ? LV_INDEV_STATE_RELEASED
                                                   : LV_INDEV_STATE_PRESSED;

    s_mouse_act = 1;
    pCtx->handleMsg = false;
  }
}

bool read_QRfbKeyEvent(vnc_context *pCtx) {
  if (pCtx->buf_len < 7)
    return false;

  ctx_dequeue(pCtx, &pCtx->key_down, 1);
  uint16_t tmp;
  ctx_dequeue(pCtx, (char *)&tmp, 2); // padding

  uint32_t key;
  ctx_dequeue(pCtx, (char *)&key, 4);
  key = ntohl(key);

  pCtx->unicode = 0;
  pCtx->keycode = 0;

  printf("key = %x\n", key);

  int i = 0;
  // while (keyMap[i].keysym && !pCtx->keycode) {
  //   if (keyMap[i].keysym == (int)key)
  //     pCtx->keycode = keyMap[i].keycode;
  //   i++;
  // }

  if (!pCtx->keycode) {
    if (key <= 0xff) {
      pCtx->unicode = key;
      if (key >= 'a' && key <= 'z')
        pCtx->keycode = /*Qt::Key_A*/ +key - 'a';
      else if (key >= ' ' && key <= '~')
        pCtx->keycode = /*Qt::Key_Space*/ +key - ' ';
    }
  }

  return true;
}

void pollkeyEvent(vnc_context *pCtx) {
  // QRfbKeyEvent ev;

  if (read_QRfbKeyEvent(pCtx)) {

    // if (pCtx->keycode == Qt::Key_Shift)
    // //   keymod =
    // //       ev.down ? keymod | Qt::ShiftModifier :
    // keymod & ~Qt::ShiftModifier;
    // // else if (ev.keycode == Qt::Key_Control)
    // //   keymod = ev.down ? keymod |
    // Qt::ControlModifier
    // //                    : keymod &
    // ~Qt::ControlModifier;
    // // else if (pCtx->.keycode == Qt::Key_Alt)
    // //   keymod = ev.down ? keymod | Qt::AltModifier :
    // keymod & ~Qt::AltModifier;
    // // if (ev.unicode || ev.keycode) {
    // //   //            qDebug() << "keyEvent" << hex <<
    // ev.unicode << ev.keycode <<
    // //   //            keymod << ev.down;
    // //   QEvent::Type type = ev.down ? QEvent::KeyPress
    // : QEvent::KeyRelease;
    // //   QString str;
    // //   if (ev.unicode && ev.unicode != 0xffff)
    // //     str = QString(ev.unicode);
    // //   QWindowSystemInterface::handleKeyEvent(0,
    // type, ev.keycode, keymod, str);
    // }
    pCtx->handleMsg = false;
  }
}

void state_connected(vnc_context *pCtx) {
  do {
    if (!pCtx->handleMsg) {
      ctx_dequeue(pCtx, &pCtx->msgType, 1);
      pCtx->handleMsg = true;
    }
    if (pCtx->handleMsg) {
      switch (pCtx->msgType) {
      case SetPixelFormat:
        // printf("Not supported: SetPixelFormat");
        setPixelFormat(pCtx);
        break;
      case FixColourMapEntries:
        // printf("Not supported: FixColourMapEntries");
        pCtx->handleMsg = false;
        break;
      case SetEncodings:
        // printf("Not supported: SetEncodings");
        set_encodings(pCtx);
        break;
      case FramebufferUpdateRequest:
        // printf("Not supported:
        // FramebufferUpdateRequest");
        frameBuffer_update_request(pCtx);
        break;
      case KeyEvent:
        printf("Not supported: KeyEvent");
        pollkeyEvent(pCtx);
        // assert(false);
        break;
      case PointerEvent:
        pointerEvent(pCtx);
        break;
      case ClientCutText:
        printf("Not supported: ClientCutText");
        // clientCutText();
        // assert(false);
        break;
      default:
        printf("Unknown message type: %d", (int)pCtx->msgType);
        pCtx->handleMsg = false;
      }
    }
  } while (!pCtx->handleMsg && pCtx->buf_len > 0);
  // write_QRfbRawEncoder(pCtx);
}
void vnc_client_process(vnc_context *pCtx) {
  int ret;
  fd_set rfds;
  struct timeval tv;
  int retval;

  const char *proto = "RFB 003.003\n";
  send(pCtx->clientFd, proto, 12, 0);
  pCtx->state = enState_Protocol;
  pCtx->buf_len = 0;
  pCtx->handleMsg = false;
  pCtx->encodingsPending = 0;
  pCtx->supportCopyRect = false;
  pCtx->supportRRE = false;
  pCtx->supportCoRRE = false;
  pCtx->supportHextile = false;
  pCtx->supportZRLE = false;
  pCtx->supportCursor = false;
  pCtx->supportDesktopSize = false;
  pCtx->encoder = NULL;
  while (pCtx->running) {
    FD_ZERO(&rfds);
    FD_SET(pCtx->clientFd, &rfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    retval = select(pCtx->clientFd + 1, &rfds, NULL, NULL, &tv);
    /* Don't rely on the value of tv now! */
    if (retval == -1) {
      perror("select()");
      break;
    } else if (retval) {
      // printf("Data is available now.\n");
      /* FD_ISSET(0, &rfds) will be true. */
      if (FD_ISSET(pCtx->clientFd, &rfds)) {
        ret = recv(pCtx->clientFd, &pCtx->buf[pCtx->buf_len],
                   256 - pCtx->buf_len, 0);
        pCtx->buf_len += ret;
        // printf("pCtx->buf_len = %d\n", pCtx->buf_len);

        switch (pCtx->state) {
        case enState_Protocol:
          state_protocol(pCtx);
          break;
        case enState_Init:
          state_init(pCtx);
          break;
        case enState_Connected:
          state_connected(pCtx);
          break;
        default:
          break;
        }
      }
    }
  }
}

void *nvcserver_mainloop(void *pParm) {
  int sockfd = -1;
  int forClientSockfd;
  struct sockaddr_in clientInfo;
  int addrlen = sizeof(clientInfo);
  printf("nvcserver_mainloop\n");
  sockfd = init_vncserver();
  while (s_vnc_running) {
    forClientSockfd = accept(sockfd, (struct sockaddr *)&clientInfo, &addrlen);
    if (forClientSockfd != -1) {
      vnc_context cxt;

      cxt.clientFd = forClientSockfd;
      cxt.serverFd = sockfd;
      cxt.running = 1;
      cxt.buf_len = 0;
      vnc_client_process(&cxt);
      printf("forClientSockfd = %d\n", forClientSockfd);
      close(forClientSockfd);
      forClientSockfd = -1;
    }
  }
  if (sockfd = -1) {
    close(sockfd);
    sockfd = -1;
  }
  return NULL;
}
