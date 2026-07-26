#include "gl/GLTextureWidget.h"
#include "tex/BcDecode.h"
#include "tex/TexFormat.h"

#include <QByteArray>
#include <QImage>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QWheelEvent>

namespace {

const char* kVert = R"(#version 450 core
layout(location = 0) in vec2 aPos;
out vec2 vUv;
uniform vec2 uScale;
uniform vec2 uOffset;   // pan, in NDC
void main() {
    // top-left origin: flip V so the texture isn't upside down
    vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = vec4(aPos * uScale + uOffset, 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 450 core
in vec2 vUv;
out vec4 FragColor;
uniform sampler2D uTex;
uniform float uUmax;     // crop the aligned row padding
uniform int   uChannels; // 0=RGBA, 1=BC4 single, 2=BC5 RG
uniform int   uChecker;  // 1 = composite over a checkerboard
void main() {
    vec2 uv = vec2(vUv.x * uUmax, vUv.y);
    vec4 c = texture(uTex, uv);
    vec3 rgb;
    float a = 1.0;
    if (uChannels == 1)      { rgb = c.rrr; }
    else if (uChannels == 2) { rgb = vec3(c.r, c.g, 1.0); }
    else                     { rgb = c.rgb; a = c.a; }
    if (uChecker == 1) {
        vec2 b = floor(gl_FragCoord.xy / 8.0);
        float chk = mod(b.x + b.y, 2.0);
        vec3 bg = mix(vec3(0.40), vec3(0.55), chk);
        FragColor = vec4(mix(bg, rgb, a), 1.0);
    } else {
        FragColor = vec4(rgb, (uChannels == 0 ? a : 1.0));
    }
}
)";

}  // namespace

GLTextureWidget::GLTextureWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    setMouseTracking(true);   // idle hover events for the pixel inspector
}

GLTextureWidget::~GLTextureWidget()
{
    if (context()) {
        makeCurrent();
        destroyTexture();
        if (m_prog) glDeleteProgram(m_prog);
        if (m_vbo)  glDeleteBuffers(1, &m_vbo);
        if (m_vao)  glDeleteVertexArrays(1, &m_vao);
        doneCurrent();
    }
}

void GLTextureWidget::setTexture(const QByteArray& bcData, int width, int height, int eTexFormat)
{
    m_data = bcData;
    m_w = width;
    m_h = height;
    m_fmt = eTexFormat;
    m_hasPending = true;
    m_error.clear();
    resetView();
    update();
}

void GLTextureWidget::setCheckerboard(bool on)
{
    m_checker = on;
    update();
}

// Inverse of the paintGL transform: widget pixel → texture UV (0..1). The on-screen
// content spans vUv [0,1] across the displayed (unpadded) image.
bool GLTextureWidget::widgetToUv(const QPoint& p, float& u, float& v) const
{
    if (!m_ready || width() <= 0 || height() <= 0) return false;
    float sx = 1.0f, sy = 1.0f;
    const float wAspect = float(width()) / float(height());
    const float tAspect = m_texH > 0 ? float(m_texW) / float(m_texH) : 1.0f;
    if (tAspect > wAspect) sy = wAspect / tAspect;
    else                   sx = tAspect / wAspect;
    sx *= m_zoom; sy *= m_zoom;
    const float ndcX = float(p.x()) / float(width()) * 2.0f - 1.0f;
    const float ndcY = 1.0f - float(p.y()) / float(height()) * 2.0f;
    if (sx == 0.0f || sy == 0.0f) return false;
    const float aposX = (ndcX - m_panX) / sx;
    const float aposY = (ndcY - m_panY) / sy;
    if (aposX < -1.0f || aposX > 1.0f || aposY < -1.0f || aposY > 1.0f) return false;
    u = aposX * 0.5f + 0.5f;
    v = 0.5f - aposY * 0.5f;
    return true;
}

void GLTextureWidget::resetView()
{
    m_zoom = 1.0f;
    m_panX = m_panY = 0.0f;
    update();
}

void GLTextureWidget::clearTexture()
{
    m_hasPending = false;
    m_data.clear();
    if (context()) {
        makeCurrent();
        destroyTexture();
        doneCurrent();
    }
    m_ready = false;
    update();
}

void GLTextureWidget::initializeGL()
{
    const bool funcsOk = initializeOpenGLFunctions();
    {
        QOpenGLContext* ctx = context();
        const QSurfaceFormat f = ctx ? ctx->format() : QSurfaceFormat();
        qInfo("GLTextureWidget GL: funcs=%d ctxValid=%d version=%d.%d profile=%d renderer=%s",
              funcsOk ? 1 : 0, (ctx && ctx->isValid()) ? 1 : 0,
              f.majorVersion(), f.minorVersion(), int(f.profile()),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    }
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);

    auto compile = [this](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            m_error = QString::fromLatin1(log);
            qWarning("GLTextureWidget shader compile failed: %s", log);
        }
        return s;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    m_prog = glCreateProgram();
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glBindAttribLocation(m_prog, 0, "aPos");
    glLinkProgram(m_prog);
    GLint linked = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(m_prog, sizeof(log), nullptr, log);
        qWarning("GLTextureWidget program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Fullscreen quad as a triangle strip.
    static const float quad[] = {-1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f};
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

void GLTextureWidget::destroyTexture()
{
    if (m_tex) {
        glDeleteTextures(1, &m_tex);
        m_tex = 0;
    }
    m_ready = false;
}

void GLTextureWidget::uploadPending()
{
    m_hasPending = false;
    destroyTexture();

    const TexFormat::Codec codec = TexFormat::codec(m_fmt, m_data.size(), m_w, m_h);
    if (!codec.valid || m_w <= 0 || m_h <= 0 || m_data.isEmpty()) {
        m_error = QStringLiteral("Unsupported format %1").arg(m_fmt);
        return;
    }

    const int   alignedW = TexFormat::alignedWidth(m_w, codec.bytesPerBlock);
    const qint64 mip0    = TexFormat::mip0Size(alignedW, m_h, codec.bytesPerBlock);
    const GLsizei imageSize =
        static_cast<GLsizei>(qMin<qint64>(mip0, m_data.size()));

    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    while (glGetError() != GL_NO_ERROR) {}   // drain
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, codec.glInternalFormat,
                           alignedW, m_h, 0, imageSize, m_data.constData());
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        // Some drivers reject certain compressed internal formats (notably BC5/BC4).
        // Fall back to a CPU decode → uncompressed RGBA upload so it still previews.
        const QImage img = BcDecode::decode(m_data, m_w, m_h, m_fmt);
        if (!img.isNull()) {
            const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
            destroyTexture();
            glGenTextures(1, &m_tex);
            glBindTexture(GL_TEXTURE_2D, m_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
            m_texW = m_w; m_texH = m_h; m_umax = 1.0f; m_channels = 0;
            m_ready = true; m_error.clear();
            return;
        }
        m_error = QStringLiteral("glCompressedTexImage2D failed (0x%1) for %2")
                      .arg(err, 0, 16).arg(codec.name);
        destroyTexture();
        return;
    }

    m_texW = m_w;
    m_texH = m_h;
    m_umax = alignedW > 0 ? float(m_w) / float(alignedW) : 1.0f;
    m_channels = (codec.glInternalFormat == TexFormat::GL_BC4) ? 1
               : (codec.glInternalFormat == TexFormat::GL_BC5) ? 2 : 0;
    m_ready = true;
    m_error.clear();
}

void GLTextureWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void GLTextureWidget::paintGL()
{
    if (m_hasPending)
        uploadPending();

    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_ready || m_prog == 0)
        return;

    // Aspect-fit (letterbox) the texture inside the widget.
    float sx = 1.0f, sy = 1.0f;
    const float wAspect = height() > 0 ? float(width()) / float(height()) : 1.0f;
    const float tAspect = m_texH > 0 ? float(m_texW) / float(m_texH) : 1.0f;
    if (tAspect > wAspect) sy = wAspect / tAspect;
    else                   sx = tAspect / wAspect;

    glUseProgram(m_prog);
    glUniform2f(glGetUniformLocation(m_prog, "uScale"), sx * m_zoom, sy * m_zoom);
    glUniform2f(glGetUniformLocation(m_prog, "uOffset"), m_panX, m_panY);
    glUniform1f(glGetUniformLocation(m_prog, "uUmax"), m_umax);
    glUniform1i(glGetUniformLocation(m_prog, "uChannels"), m_channels);
    glUniform1i(glGetUniformLocation(m_prog, "uChecker"), m_checker ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

QImage GLTextureWidget::grabImage()
{
    if (m_prog == 0)          // GL never initialized (tab never shown)
        return QImage();

    makeCurrent();
    if (m_hasPending)         // upload now if a setTexture hasn't been painted yet
        uploadPending();
    if (!m_ready || m_texW <= 0 || m_texH <= 0) {
        doneCurrent();
        return QImage();
    }

    // Render the texture 1:1 into an offscreen RGBA buffer at full resolution.
    QOpenGLFramebufferObject fbo(m_texW, m_texH);
    fbo.bind();
    glViewport(0, 0, m_texW, m_texH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_prog);
    glUniform2f(glGetUniformLocation(m_prog, "uScale"), 1.0f, 1.0f);  // fill the FBO
    glUniform2f(glGetUniformLocation(m_prog, "uOffset"), 0.0f, 0.0f);
    glUniform1f(glGetUniformLocation(m_prog, "uUmax"), m_umax);
    glUniform1i(glGetUniformLocation(m_prog, "uChannels"), m_channels);
    glUniform1i(glGetUniformLocation(m_prog, "uChecker"), 0);   // never bake the checker in
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    fbo.release();
    QImage img = fbo.toImage();   // top-down RGBA, oriented as displayed
    doneCurrent();
    update();                     // restore the on-screen view
    return img;
}

void GLTextureWidget::wheelEvent(QWheelEvent* e)
{
    if (!m_ready) return;
    const float f = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    m_zoom = qBound(0.1f, m_zoom * f, 40.0f);
    if (m_zoom <= 1.001f && m_zoom >= 0.999f) { m_panX = m_panY = 0.0f; }
    update();
    e->accept();
}

void GLTextureWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastPos = e->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void GLTextureWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging) {
        const QPoint d = e->pos() - m_lastPos;
        m_lastPos = e->pos();
        if (width() > 0)  m_panX += 2.0f * float(d.x()) / float(width());
        if (height() > 0) m_panY -= 2.0f * float(d.y()) / float(height());
        update();
        return;
    }
    float u, v;
    if (widgetToUv(e->pos(), u, v)) emit hoverUv(QPointF(u, v));
    else                            emit hoverUv(QPointF(-1, -1));
}

void GLTextureWidget::leaveEvent(QEvent* e)
{
    Q_UNUSED(e);
    emit hoverUv(QPointF(-1, -1));
}

void GLTextureWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
    }
}

void GLTextureWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    Q_UNUSED(e);
    resetView();   // double-click resets zoom/pan
    setCursor(Qt::ArrowCursor);
    m_dragging = false;
}
