// TITAN <-> Unreal coordinate conversion. Original TITAN code.
//
// Plain C++ (no Unreal types) so the same code is compiled into the Unreal bridge and into TITAN's own
// unit tests (tests/test_ue_coordinates.cpp).
//
//   Unreal: left-handed, Z-up, X forward, Y right, centimetres. FRotator(Pitch, Yaw, Roll) in degrees,
//           applied roll (X), then pitch (Y), then yaw (Z).
//   TITAN:  right-handed, Y-up, -Z forward, X right, metres. Euler degrees, R = Ry * Rx * Rz.
//
//   Axis map M (det = -1, the handedness flip):  titan = (ue.Y, ue.Z, -ue.X) / 100
//   Rotations are conjugated by M:  R_titan = M * R_ue * M^-1   (exact for every rotation).
//   Scale is per local axis:        titan scale = (ue.Y, ue.Z, ue.X)
#pragma once

#include <cmath>

namespace TitanCoords
{
struct V3
{
	double X = 0, Y = 0, Z = 0;
};

// Row-major 3x3, M[r][c]; matrices act on column vectors.
struct M3
{
	double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	V3 operator*(const V3& V) const
	{
		return {M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z, M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
				M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z};
	}
	M3 operator*(const M3& B) const
	{
		M3 R;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				R.M[r][c] = M[r][0] * B.M[0][c] + M[r][1] * B.M[1][c] + M[r][2] * B.M[2][c];
		return R;
	}
	M3 Transposed() const
	{
		M3 R;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c) R.M[r][c] = M[c][r];
		return R;
	}
};

constexpr double kPi = 3.14159265358979323846;
inline double Rad(double D) { return D * kPi / 180.0; }
inline double Deg(double R) { return R * 180.0 / kPi; }

// ---- positions / directions / scale -------------------------------------------------------
inline V3 UeToTitanDirection(const V3& U) { return {U.Y, U.Z, -U.X}; }
inline V3 TitanToUeDirection(const V3& T) { return {-T.Z, T.X, T.Y}; }
inline V3 UeToTitanPosition(const V3& U) { return {U.Y / 100.0, U.Z / 100.0, -U.X / 100.0}; }
inline V3 TitanToUePosition(const V3& T) { return {-T.Z * 100.0, T.X * 100.0, T.Y * 100.0}; }
inline V3 UeToTitanScale(const V3& U) { return {U.Y, U.Z, U.X}; }
inline V3 TitanToUeScale(const V3& T) { return {T.Z, T.X, T.Y}; }

inline M3 AxisMap()  // M: UE -> TITAN
{
	M3 A;
	A.M[0][0] = 0; A.M[0][1] = 1; A.M[0][2] = 0;
	A.M[1][0] = 0; A.M[1][1] = 0; A.M[1][2] = 1;
	A.M[2][0] = -1; A.M[2][1] = 0; A.M[2][2] = 0;
	return A;
}

// ---- Unreal rotator <-> matrix (same convention as FRotationMatrix / FMatrix::Rotator) -------
// Columns of the result are the rotated UE X, Y, Z axes.
inline M3 UeRotatorToMatrix(double Pitch, double Yaw, double Roll)
{
	const double SP = std::sin(Rad(Pitch)), CP = std::cos(Rad(Pitch));
	const double SY = std::sin(Rad(Yaw)), CY = std::cos(Rad(Yaw));
	const double SR = std::sin(Rad(Roll)), CR = std::cos(Rad(Roll));
	// Rows of FRotationMatrix are the rotated axes (UE uses row vectors); transpose into columns.
	const double X[3] = {CP * CY, CP * SY, SP};
	const double Y[3] = {SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP};
	const double Z[3] = {-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP};
	M3 R;
	for (int i = 0; i < 3; ++i)
	{
		R.M[i][0] = X[i];
		R.M[i][1] = Y[i];
		R.M[i][2] = Z[i];
	}
	return R;
}

inline void UeMatrixToRotator(const M3& R, double& Pitch, double& Yaw, double& Roll)
{
	const V3 X{R.M[0][0], R.M[1][0], R.M[2][0]};
	const V3 Y{R.M[0][1], R.M[1][1], R.M[2][1]};
	const V3 Z{R.M[0][2], R.M[1][2], R.M[2][2]};
	Pitch = Deg(std::atan2(X.Z, std::sqrt(X.X * X.X + X.Y * X.Y)));
	Yaw = Deg(std::atan2(X.Y, X.X));
	// Roll: compare the actual Y/Z axes with the Y axis of the roll-free rotation (as FMatrix::Rotator).
	const M3 NoRoll = UeRotatorToMatrix(Pitch, Yaw, 0.0);
	const V3 SYAxis{NoRoll.M[0][1], NoRoll.M[1][1], NoRoll.M[2][1]};
	const double ZDot = Z.X * SYAxis.X + Z.Y * SYAxis.Y + Z.Z * SYAxis.Z;
	const double YDot = Y.X * SYAxis.X + Y.Y * SYAxis.Y + Y.Z * SYAxis.Z;
	Roll = Deg(std::atan2(ZDot, YDot));
}

// ---- TITAN Euler <-> matrix (R = Ry * Rx * Rz, matching titan::Transform::matrix) -----------
inline M3 TitanEulerToMatrix(double XDeg, double YDeg, double ZDeg)
{
	const double cx = std::cos(Rad(XDeg)), sx = std::sin(Rad(XDeg));
	const double cy = std::cos(Rad(YDeg)), sy = std::sin(Rad(YDeg));
	const double cz = std::cos(Rad(ZDeg)), sz = std::sin(Rad(ZDeg));
	M3 R;
	R.M[0][0] = cy * cz + sy * sx * sz; R.M[0][1] = -cy * sz + sy * sx * cz; R.M[0][2] = sy * cx;
	R.M[1][0] = cx * sz;                R.M[1][1] = cx * cz;                 R.M[1][2] = -sx;
	R.M[2][0] = -sy * cz + cy * sx * sz; R.M[2][1] = sy * sz + cy * sx * cz; R.M[2][2] = cy * cx;
	return R;
}

inline void TitanMatrixToEuler(const M3& R, double& XDeg, double& YDeg, double& ZDeg)
{
	const double S = -R.M[1][2];
	if (std::fabs(S) < 0.9999999)
	{
		XDeg = Deg(std::asin(S));
		YDeg = Deg(std::atan2(R.M[0][2], R.M[2][2]));
		ZDeg = Deg(std::atan2(R.M[1][0], R.M[1][1]));
	}
	else  // gimbal lock: X = +-90 deg, fold Z into Y
	{
		XDeg = S > 0 ? 90.0 : -90.0;
		YDeg = Deg(std::atan2(-R.M[2][0], R.M[0][0]));
		ZDeg = 0.0;
	}
}

// ---- the conversions the bridge uses ------------------------------------------------------
inline V3 UeRotatorToTitanEuler(double Pitch, double Yaw, double Roll)
{
	const M3 A = AxisMap();
	const M3 T = A * UeRotatorToMatrix(Pitch, Yaw, Roll) * A.Transposed();
	V3 E;
	TitanMatrixToEuler(T, E.X, E.Y, E.Z);
	return E;
}

inline void TitanEulerToUeRotator(const V3& E, double& Pitch, double& Yaw, double& Roll)
{
	const M3 A = AxisMap();
	const M3 U = A.Transposed() * TitanEulerToMatrix(E.X, E.Y, E.Z) * A;
	UeMatrixToRotator(U, Pitch, Yaw, Roll);
}

// Camera: TITAN eye + forward (unit) -> UE location (cm) and rotator (roll 0).
inline void TitanCameraToUe(const V3& Eye, const V3& Forward, V3& OutLocation, double& Pitch, double& Yaw)
{
	OutLocation = TitanToUePosition(Eye);
	const V3 F = TitanToUeDirection(Forward);
	Yaw = Deg(std::atan2(F.Y, F.X));
	Pitch = Deg(std::atan2(F.Z, std::sqrt(F.X * F.X + F.Y * F.Y)));
}

// Scene captures take a horizontal field of view; TITAN Studio uses a vertical one.
inline double HorizontalFovDeg(double VerticalFovDeg, double Aspect)
{
	return Deg(2.0 * std::atan(std::tan(Rad(VerticalFovDeg) * 0.5) * Aspect));
}
}  // namespace TitanCoords
