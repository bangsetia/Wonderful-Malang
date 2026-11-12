#pragma once
#include <cmath>
#include <algorithm>
#include "imgui.h"

struct Vector3D {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3D() = default;
    Vector3D(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vector3D operator+(const Vector3D& other) const { return { x + other.x, y + other.y, z + other.z }; }
    Vector3D operator-(const Vector3D& other) const { return { x - other.x, y - other.y, z - other.z }; }
    Vector3D operator*(float scalar) const { return { x * scalar, y * scalar, z * scalar }; }

    float DistanceTo(const Vector3D& other) const {
        return std::sqrt((x - other.x) * (x - other.x) +
            (y - other.y) * (y - other.y) +
            (z - other.z) * (z - other.z));
    }
};

struct Camera3D {
    Vector3D position = { 0.0f, 15.0f, -30.0f };
    float pos[3] = { 0.0f, 20.0f, -60.0f };
    float yaw = 0.0f;
    float pitch = -0.3f;
    bool isFlightActive = false;
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

struct RenderPipeline {
    static void GetViewProjectionMatrix(const Camera3D& cam, float aspect, float outMatrix[4][4]) {
        if (aspect <= 0.0f) aspect = 1.0f;
        const float cosYaw = std::cosf(cam.yaw);
        const float sinYaw = std::sinf(cam.yaw);
        const float cosPitch = std::cosf(cam.pitch);
        const float sinPitch = std::sinf(cam.pitch);
        const Vector3D look = { cosPitch * sinYaw, sinPitch, cosPitch * cosYaw };
        const Vector3D right = { cosYaw, 0.0f, -sinYaw };
        const Vector3D up = {
            right.y * look.z - right.z * look.y,
            right.z * look.x - right.x * look.z,
            right.x * look.y - right.y * look.x
        };

        const float view[4][4] = {
            { right.x, up.x, look.x, 0.0f },
            { right.y, up.y, look.y, 0.0f },
            { right.z, up.z, look.z, 0.0f },
            { -(cam.position.x * right.x + cam.position.y * right.y + cam.position.z * right.z),
              -(cam.position.x * up.x + cam.position.y * up.y + cam.position.z * up.z),
              -(cam.position.x * look.x + cam.position.y * look.y + cam.position.z * look.z), 1.0f }
        };
        const float r = 1.0f / std::tanf(cam.fov * 0.5f * 3.14159265f / 180.0f);
        const float denominator = cam.farPlane - cam.nearPlane;
        const float fRange = denominator > 0.0f ? cam.farPlane / denominator : 1.0f;
        const float projection[4][4] = {
            { r / aspect, 0.0f, 0.0f, 0.0f },
            { 0.0f, r, 0.0f, 0.0f },
            { 0.0f, 0.0f, fRange, 1.0f },
            { 0.0f, 0.0f, -fRange * cam.nearPlane, 0.0f }
        };

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                outMatrix[row][column] = view[row][0] * projection[0][column] +
                    view[row][1] * projection[1][column] +
                    view[row][2] * projection[2][column] +
                    view[row][3] * projection[3][column];
            }
        }
    }

    static bool ProjectWorldToScreen(const Vector3D& worldPos, const float matVP[4][4],
        float viewWidth, float viewHeight, ImVec2& outScreen) {
        const float w = worldPos.x * matVP[0][3] + worldPos.y * matVP[1][3] +
            worldPos.z * matVP[2][3] + matVP[3][3];
        if (w <= 0.01f || viewWidth <= 0.0f || viewHeight <= 0.0f) return false;
        const float x = worldPos.x * matVP[0][0] + worldPos.y * matVP[1][0] +
            worldPos.z * matVP[2][0] + matVP[3][0];
        const float y = worldPos.x * matVP[0][1] + worldPos.y * matVP[1][1] +
            worldPos.z * matVP[2][1] + matVP[3][1];
        outScreen.x = (1.0f + x / w) * 0.5f * viewWidth;
        outScreen.y = (1.0f - y / w) * 0.5f * viewHeight;
        return true;
    }
};

class EngineMath {
public:
    // Formula Hermite Spline dengan nama variabel yang unik untuk menghindari tabrakan makro internal SDK
    static Vector3D InterpolateSpline(const Vector3D& p0, float rotationVal0, const Vector3D& p1, float rotationVal1, float t_factor) {

        // Menggunakan nama panjang agar 100% aman dari kesalahan syntax error: 'constant'
        float radiansVal0 = rotationVal0 * 0.0174532925f;
        float radiansVal1 = rotationVal1 * 0.0174532925f;

        float curveDistance = p0.DistanceTo(p1) * 0.5f;

        // Kalkulasi vektor arah tangent jalan murni float
        Vector3D tangent0(std::sinf(radiansVal0) * curveDistance, -std::cosf(radiansVal0) * curveDistance, 0.0f);
        Vector3D tangent1(std::sinf(radiansVal1) * curveDistance, -std::cosf(radiansVal1) * curveDistance, 0.0f);

        // Polinomial Hermite Basis
        float t2 = t_factor * t_factor;
        float t3 = t2 * t_factor;

        float h1 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        float h2 = -2.0f * t3 + 3.0f * t2;
        float h3 = t3 - 2.0f * t2 + t_factor;
        float h4 = t3 - t2;

        return Vector3D(
            h1 * p0.x + h2 * p1.x + h3 * tangent0.x + h4 * tangent1.x,
            h1 * p0.y + h2 * p1.y + h3 * tangent0.y + h4 * tangent1.y,
            h1 * p0.z + h2 * p1.z
        );
    }
};
