#include "geodesic_voronoi_grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::normalized;

static constexpr double R = 3500000.0;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

struct Result {
	double relative_rms = 0.0;
	double max_abs = 0.0;
	double max_metric_skew = 0.0;
	double max_cell_kite_error = 0.0;
	double max_vertex_kite_error = 0.0;
};

static Result evaluate(int frequency) {
	GeodesicVoronoiGrid grid(frequency, R);
	const Vec3d axis = normalized(Vec3d{0.37, 0.81, -0.455});
	const Vec3d axis2 = normalized(Vec3d{-0.62, 0.24, 0.747});
	std::vector<double> normal(static_cast<size_t>(grid.edge_count()), 0.0);
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		const Vec3d field = cross(axis, edge.midpoint) * 47.0
			+ cross(axis2, edge.midpoint) * 13.0;
		normal[static_cast<size_t>(e)] = dot(field, edge.normal_a_to_b);
	}

	long double error2 = 0.0L;
	long double reference2 = 0.0L;
	double max_abs = 0.0;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		double reconstructed = 0.0;
		for (size_t k = 0; k < edge.reconstruction_edges.size(); ++k) {
			reconstructed += edge.reconstruction_weights[k]
				* normal[static_cast<size_t>(edge.reconstruction_edges[k])];
		}
		const Vec3d field = cross(axis, edge.midpoint) * 47.0
			+ cross(axis2, edge.midpoint) * 13.0;
		const double expected = dot(field, edge.tangent_a_to_b);
		const double error = reconstructed - expected;
		error2 += static_cast<long double>(edge.edge_area_m2) * error * error;
		reference2 += static_cast<long double>(edge.edge_area_m2) * expected * expected;
		max_abs = std::max(max_abs, std::abs(error));
	}

	double max_skew = 0.0;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &target = grid.edge(e);
		const double m_e = target.edge_area_m2;
		for (size_t k = 0; k < target.reconstruction_edges.size(); ++k) {
			const int s = target.reconstruction_edges[k];
			const auto &source = grid.edge(s);
			double reverse = 0.0;
			bool found = false;
			for (size_t j = 0; j < source.reconstruction_edges.size(); ++j) {
				if (source.reconstruction_edges[j] == e) {
					reverse = source.reconstruction_weights[j];
					found = true;
					break;
				}
			}
			require(found, "TRiSK reconstruction relation is not reciprocal");
			const double lhs = m_e * target.reconstruction_weights[k]
				+ source.edge_area_m2 * reverse;
			const double scale = std::max({std::abs(m_e * target.reconstruction_weights[k]),
				std::abs(source.edge_area_m2 * reverse), 1.0});
			max_skew = std::max(max_skew, std::abs(lhs) / scale);
		}
	}

	double max_cell_kite = 0.0;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto &cell = grid.cell(c);
		long double sum = 0.0L;
		for (double a : cell.kite_area_m2) sum += a;
		max_cell_kite = std::max(max_cell_kite,
			std::abs(static_cast<double>(sum) - cell.area_m2) / cell.area_m2);
	}
	double max_vertex_kite = 0.0;
	for (int v = 0; v < grid.vertex_count(); ++v) {
		const auto &vertex = grid.vertex(v);
		const double sum = vertex.kite_area_m2[0] + vertex.kite_area_m2[1] + vertex.kite_area_m2[2];
		max_vertex_kite = std::max(max_vertex_kite,
			std::abs(sum - vertex.dual_area_m2) / vertex.dual_area_m2);
	}

	Result out;
	out.relative_rms = std::sqrt(static_cast<double>(error2 / std::max(reference2, 1.0L)));
	out.max_abs = max_abs;
	out.max_metric_skew = max_skew;
	out.max_cell_kite_error = max_cell_kite;
	out.max_vertex_kite_error = max_vertex_kite;
	return out;
}

int main() {
	try {
		const Result f6 = evaluate(6);
		const Result f12 = evaluate(12);
		const Result f24 = evaluate(24);

		require(f6.max_cell_kite_error < 2e-10 && f12.max_cell_kite_error < 2e-10
			&& f24.max_cell_kite_error < 2e-10, "TRiSK primal kite areas do not close");
		require(f6.max_vertex_kite_error < 2e-10 && f12.max_vertex_kite_error < 2e-10
			&& f24.max_vertex_kite_error < 2e-10, "TRiSK dual kite areas do not close");
		require(f24.max_metric_skew < 5e-11, "TRiSK weights violate kinetic-energy metric skew symmetry");
		require(f12.relative_rms < f6.relative_rms * 0.80,
			"TRiSK tangential reconstruction does not converge from F6 to F12");
		require(f24.relative_rms < f12.relative_rms * 0.80,
			"TRiSK tangential reconstruction does not converge from F12 to F24");
		require(f24.relative_rms < 0.035,
			"TRiSK tangential reconstruction remains too inaccurate at F24");

		std::cout << "TRiSK weights PASS\n"
			<< "  F6 relative RMS/max: " << f6.relative_rms << " " << f6.max_abs << "\n"
			<< "  F12 relative RMS/max: " << f12.relative_rms << " " << f12.max_abs << "\n"
			<< "  F24 relative RMS/max: " << f24.relative_rms << " " << f24.max_abs << "\n"
			<< "  max metric-skew residual: " << f24.max_metric_skew << "\n"
			<< "  max cell/vertex kite closure: " << f24.max_cell_kite_error
			<< " " << f24.max_vertex_kite_error << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "TRiSK weights FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
