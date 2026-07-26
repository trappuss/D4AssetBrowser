#pragma once
#include <QByteArray>
#include <QImage>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

// Uploads a compressed BC texture payload straight to the GPU with
// glCompressedTexImage2D and draws it aspect-fit — no CPU decode. This is exactly
// how d4analyzer achieves instant texture previews (its binary links QOpenGLWidget +
// QOpenGLFunctions_4_5_Core). Ported from the Python fork's gl_texture.py.
class GLTextureWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core {
    Q_OBJECT
public:
    explicit GLTextureWidget(QWidget* parent = nullptr);
    ~GLTextureWidget() override;

    // Upload the top mip of a BC payload. eTexFormat picks the GL internal format
    // and the 256-byte aligned row width. Safe to call from the GUI thread.
    void setTexture(const QByteArray& bcData, int width, int height, int eTexFormat);
    void clearTexture();

    // Render the uploaded texture to an offscreen FBO at full resolution and read
    // it back as a QImage (the GPU does the BC decode). Null if nothing is loaded.
    QImage grabImage();
    bool hasTexture() const { return m_ready; }

    void setCheckerboard(bool on);   // show a checker behind transparent pixels
    void resetView();                // zoom 1, no pan

signals:
    // Texture-space UV (0..1) under the cursor, or (-1,-1) when outside the image.
    void hoverUv(QPointF uv);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void uploadPending();
    void destroyTexture();

    // pending upload (applied on the next paintGL with a current context)
    bool       m_hasPending = false;
    QByteArray m_data;
    int        m_w = 0, m_h = 0, m_fmt = 0;

    // GL objects / state
    GLuint  m_prog = 0, m_vao = 0, m_vbo = 0, m_tex = 0;
    int     m_texW = 0, m_texH = 0;   // actual (unpadded) dimensions for aspect fit
    int     m_channels = 0;           // 0=RGBA, 1=BC4 (R→grey), 2=BC5 (RG)
    float   m_umax = 1.0f;            // actualWidth / alignedWidth (crops row padding)
    bool    m_ready = false;
    QString m_error;

    // View transform (zoom/pan) + alpha checkerboard.
    float   m_zoom = 1.0f;
    float   m_panX = 0.0f, m_panY = 0.0f;   // NDC offset
    bool    m_checker = false;
    bool    m_dragging = false;
    QPoint  m_lastPos;

    // Map a widget point to texture UV (0..1), honoring aspect-fit + zoom/pan.
    bool widgetToUv(const QPoint& p, float& u, float& v) const;
};
