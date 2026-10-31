/* N-body simulation: structs of doubles, sqrt, division. */
#include <math.h>
#include <stdio.h>

typedef struct { double x, y, z, vx, vy, vz, mass; } body_t;

#define BODIES 5
static body_t bodies[BODIES] = {
    {0, 0, 0, 0, 0, 0, 39.47841760435743},
    {4.84143144246472090, -1.16032004402742839, -0.103622044471123109, 0.606326392995832, 2.81198684491626, -0.02521836165988763, 0.03769367487038949},
    {8.34336671824457987, 4.12479856412430479, -0.403523417114321381, -1.01077434617730, 1.82566237123041, 0.008415761376584154, 0.011286326131968767},
    {12.8943695621391310, -15.1111514016986312, -0.223307578892655734, 1.08279100644154, 0.868713018169608, -0.01083270493846, 0.0017237240570597112},
    {15.3796971148509165, -25.9193146099879641, 0.179258772950371181, 0.979090732243898, 0.594698998647676, -0.034755955504078, 0.0020336868699246304},
};

static double energy(void)
{
    double e = 0.0;
    for (int i = 0; i < BODIES; i++) {
        body_t *b = &bodies[i];
        e += 0.5 * b->mass * (b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
        for (int j = i + 1; j < BODIES; j++) {
            double dx = b->x - bodies[j].x, dy = b->y - bodies[j].y, dz = b->z - bodies[j].z;
            e -= b->mass * bodies[j].mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void advance(double dt)
{
    for (int i = 0; i < BODIES; i++) {
        body_t *b = &bodies[i];
        for (int j = i + 1; j < BODIES; j++) {
            body_t *c = &bodies[j];
            double dx = b->x - c->x, dy = b->y - c->y, dz = b->z - c->z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            b->vx -= dx * c->mass * mag; b->vy -= dy * c->mass * mag; b->vz -= dz * c->mass * mag;
            c->vx += dx * b->mass * mag; c->vy += dy * b->mass * mag; c->vz += dz * b->mass * mag;
        }
    }
    for (int i = 0; i < BODIES; i++) {
        bodies[i].x += dt * bodies[i].vx;
        bodies[i].y += dt * bodies[i].vy;
        bodies[i].z += dt * bodies[i].vz;
    }
}

int main(void)
{
    printf("nbody %.9f", energy());
    for (int i = 0; i < 2000000; i++)
        advance(0.01);
    printf(" %.9f\n", energy());
    return 0;
}
