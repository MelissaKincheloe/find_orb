/* ephem0.cpp: low-level funcs for ephemerides & pseudo-MPECs

Copyright (C) 2010, Project Pluto

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.    */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>
#include <stdbool.h>
#include "watdefs.h"
#include "afuncs.h"
#include "lunar.h"
#include "date.h"
#include "comets.h"
#include "mpc_obs.h"

#define J2000 2451545.0
#define EARTH_MAJOR_AXIS 6378140.
#define EARTH_MAJOR_AXIS_IN_AU (EARTH_MAJOR_AXIS / AU_IN_METERS)
#define PI 3.1415926535897932384626433832795028841971693993751058209749445923
#define LOG_10 2.3025850929940456840179914546843642076011014886287729760333279009675726
#define LIGHT_YEAR_IN_KM    (365.25 * seconds_per_day * SPEED_OF_LIGHT)

#define ROB_MATSON_TEST_CODE     1

double centralize_ang( double ang);             /* elem_out.cpp */
double vector_to_polar( double *lon, double *lat, const double *vector);
char *fgets_trimmed( char *buff, size_t max_bytes, FILE *ifile); /*elem_out.c*/
int parallax_to_lat_alt( const double rho_cos_phi, const double rho_sin_phi,
       double *lat, double *ht_in_meters, const int planet_idx); /* ephem0.c */
double calc_obs_magnitude( const int is_comet, const double obj_sun,
          const double obj_earth, const double earth_sun, double *phase_ang);
int lat_alt_to_parallax( const double lat, const double ht_in_meters,
             double *rho_cos_phi, double *rho_sin_phi, const int planet_idx);
int write_residuals_to_file( const char *filename, const char *ast_filename,
          const int n_obs, const OBSERVE FAR *obs_data, const int format);
void put_observer_data_in_text( const char FAR *mpc_code, char *buff);
double dot_product( const double *v1, const double *v2);       /* sr.cpp */
int make_pseudo_mpec( const char *mpec_filename, const char *obj_name);
                                              /* ephem0.cpp */
int earth_lunar_posn( const double jd, double FAR *earth_loc,
                                       double FAR *lunar_loc);
void remove_trailing_cr_lf( char *buff);      /* ephem0.cpp */
void create_obs_file( const OBSERVE FAR *obs, int n_obs, const int append);
const char *get_environment_ptr( const char *env_ptr);     /* mpc_obs.cpp */
void set_environment_ptr( const char *env_ptr, const char *new_value);
int text_search_and_replace( char FAR *str, const char *oldstr,
                                     const char *newstr);   /* ephem0.cpp */
void format_dist_in_buff( char *buff, const double dist_in_au); /* ephem0.c */
char int_to_mutant_hex_char( const int ival);              /* mpc_obs.c */
int debug_printf( const char *format, ...);                /* mpc_obs.c */
double planet_axis_ratio( const int planet_idx);            /* collide.cpp */
double planet_radius_in_meters( const int planet_idx);      /* collide.cpp */
double mag_band_shift( const char mag_band);                /* elem_out.c */
char *get_file_name( char *filename, const char *template_file_name);
double current_jd( void);                       /* elem_out.cpp */
double diameter_from_abs_mag( const double abs_mag,      /* ephem0.cpp */
                                     const double optical_albedo);
int snprintf_append( char *string, const size_t max_len,      /* ephem0.cpp */
                                   const char *format, ...);
double find_moid( const ELEMENTS *elem1, const ELEMENTS *elem2,  /* moid4.c */
                                     double *barbee_style_delta_v);
int setup_planet_elem( ELEMENTS *elem, const int planet_idx,
                                          const double t_cen);   /* moid4.c */
char *mpc_station_name( char *station_data);       /* mpc_obs.cpp */
FILE *fopen_ext( const char *filename, const char *permits);   /* miscell.cpp */

const char *observe_filename = "observe.txt";
const char *residual_filename = "residual.txt";
const char *ephemeris_filename = "ephemeri.txt";
const char *elements_filename = "elements.txt";

/* Returns parallax constants (rho_cos_phi, rho_sin_phi) in AU. */

int lat_alt_to_parallax( const double lat, const double ht_in_meters,
            double *rho_cos_phi, double *rho_sin_phi, const int planet_idx)
{
   const double axis_ratio = planet_axis_ratio( planet_idx);
   const double major_axis_in_meters = planet_radius_in_meters( planet_idx);
   const double u = atan( sin( lat) * axis_ratio / cos( lat));

   *rho_sin_phi = axis_ratio * sin( u) +
                            (ht_in_meters / major_axis_in_meters) * sin( lat);
   *rho_cos_phi = cos( u) + (ht_in_meters / major_axis_in_meters) * cos( lat);
   *rho_sin_phi *= major_axis_in_meters / AU_IN_METERS;
   *rho_cos_phi *= major_axis_in_meters / AU_IN_METERS;
   return( 0);
}

/* Takes parallax constants (rho_cos_phi, rho_sin_phi) in units of the
equatorial radius.  The previous non-iterative version of this function,
used before 2016 Oct, was taken from Meeus' _Astronomical Algorithms_.  It
could be off by as much as 16 meters in altitude and wrong in latitude by
about three meters for each km of altitude (i.e.,  about 25 meters at the
top of Everest).  Things are worse for planets more oblate than the earth.

   An exact non-iterative solution exists,  but is somewhat complicated
(requires finding zeroes of a quartic polynomial).  The iterative
solution given below is faster and simpler.  It starts out with a
laughably poor guess,  but convergence is fast;  eight iterations gets
sub-micron accuracy. */

int parallax_to_lat_alt( const double rho_cos_phi, const double rho_sin_phi,
               double *lat, double *ht_in_meters, const int planet_idx)
{
   const double major_axis_in_meters = planet_radius_in_meters( planet_idx);
   const double lat0 = atan2( rho_sin_phi, rho_cos_phi);
   const double rho0 = sqrt( rho_sin_phi * rho_sin_phi + rho_cos_phi * rho_cos_phi)
                     * major_axis_in_meters / AU_IN_METERS;
   double tlat = lat0, talt = 0.;
   int iter;

// printf( "Input parallax constants : %+.9f %.9f\n", rho_sin_phi, rho_cos_phi);
   for( iter = 0; iter < 8; iter++)
      {
      double rc2, rs2;

//    printf( "Curr lat/alt: %.9f %.9f\n", tlat * 180. / PI, talt);
      lat_alt_to_parallax( tlat, talt, &rc2, &rs2, planet_idx);
      talt -= (sqrt( rs2 * rs2 + rc2 * rc2) - rho0) * AU_IN_METERS;
      tlat -= atan2( rs2, rc2) - lat0;
      }
   if( lat)
      *lat = tlat;
   if( ht_in_meters)
      *ht_in_meters = talt;
   return( 0);
}

#include <stdarg.h>
#ifdef _MSC_VER
     /* Microsoft Visual C/C++ has no snprintf.  Yes,  you read that      */
     /* correctly.  MSVC has an _snprintf which doesn't add a '\0' at the */
     /* end if max_len bytes are written.  You can't pass a NULL string   */
     /* to determine the necessary buffer size.  The following, however,  */
     /* is a "good enough" replacement:  for a non-NULL string, the       */
     /* output will be "correct" (the '\0' will always be present and in  */
     /* the right place).  The only deviation from "proper" snprintf() is */
     /* that the maximum return value is max_len;  you can never know how */
     /* many bytes "should" have been written.                            */
     /*   Somewhat ancient MSVCs don't even have vsnprintf;  you have to  */
     /* use vsprintf and work things out so you aren't overwriting the    */
     /* end of the buffer.                                                */

int snprintf( char *string, const size_t max_len, const char *format, ...)
{
   va_list argptr;
   int rval;

   va_start( argptr, format);
#if _MSC_VER <= 1100
   rval = vsprintf( string, format, argptr);
#else
   rval = vsnprintf( string, max_len, format, argptr);
#endif
   string[max_len - 1] = '\0';
   va_end( argptr);
   return( rval);
}
#endif    /* #ifdef _MSC_VER */

/* I find myself frequently snprintf()-ing at the end of a string,  with    */
/* something like snprintf( str + strlen( str), sizeof( str) - strlen(str), */
/* ...).  This should be a little more convenient.                          */

int snprintf_append( char *string, const size_t max_len,      /* ephem0.cpp */
                                   const char *format, ...)
{
   va_list argptr;
   int rval;
   const size_t ilen = strlen( string);

   assert( ilen <= max_len);
   va_start( argptr, format);
#if _MSC_VER <= 1100
   rval = vsprintf( string + ilen, format, argptr);
#else
   rval = vsnprintf( string + ilen, max_len - ilen, format, argptr);
#endif
   string[max_len - 1] = '\0';
   va_end( argptr);
   return( rval);
}

/* format_dist_in_buff() formats the input distance (in AU) into a
seven-byte buffer.  It does this by choosing suitable units: kilometers
if the distance is less than a million km,  AU out to 10000 AU,  then
light-years.  In actual use,  light-years indicates some sort of error,
but I wanted the program to handle that problem without just crashing.

   28 Dec 2009:  To help puzzle out a strange bug,  I extended things so
that distances beyond 10000 light-years could be shown in kilo-light-years
(KLY),  mega,  giga,  etc. units,  making up prefixes after 'yocto'.  Thus,
one "ALY" = 10^78 light-years.  Since the universe is 13.7 billion years
old,  and nothing can be seen beyond that point,  such distances are
physically unreasonable by a factor of roughly 10^68.  But for debugging
purposes,  display of these distances was useful.  The bug is now fixed,
so with any luck,  nobody should see such distances again.

   2012 Feb 13:  there's some interest in determining orbits of _very_
close objects,  such as tennis balls.  To address this,  short distances
are now shown in millimeters,  centimeters,  meters,  or .1 km,
as appropriate.

NOTE:  It used to be that I followed MPC practice in showing all distances
between .01 and ten AU in the form d.dddd , but now,  that's only true for
one to ten AU.  For distances less than 1 AU,  I'm using .ddddd,  thereby
getting an extra digit displayed.   */

static void show_dist_in_au( char *buff, const double dist_in_au)
{
   const char *fmt;

   if( dist_in_au > 999.999)
      fmt = "%7.1f";             /* " 1234.5" */
   else if( dist_in_au > 99.999)
      fmt = "%7.2f";             /* " 123.45" */
   else if( dist_in_au > 9.999)
      fmt = "%7.3f";             /* " 12.345" */
   else if( dist_in_au > .99)     /* used to be .01 */
      fmt = "%7.4f";             /* " 1.2345" */
   else
      fmt = "%7.5f";             /* " .12345" */
   sprintf( buff, fmt, dist_in_au);
   *buff = ' ';   /* remove leading zero for small amounts */
}

static const char *si_prefixes = "kMGTPEZYXWVUSRQONLJIHFDCBA";
static bool use_au_only = false;

static void show_packed_with_si_prefixes( char *buff, double ival)
{
   *buff = '\0';
   if( ival > 999.e+21)
      strcpy( buff, "!!!!");
   else if( ival > 9999.)
      {
      unsigned count = 0;

      *buff = '\0';
      while( !*buff)
         {
         ival /= 1000.;
         if( ival < 9.9)
            sprintf( buff, "%3.1f%c", ival, si_prefixes[count]);
         else if( ival < 999.)
            sprintf( buff, "%3u%c", (unsigned)ival, si_prefixes[count]);
         count++;
         }
      }
   else if( ival > 99.9)
      sprintf( buff, "%4u", (unsigned)( ival + .5));
   else if( ival > 9.9)
      sprintf( buff, "%4.1f", ival);
   else if( ival > .99)
      sprintf( buff, "%4.2f", ival);
   else
      {
      char tbuff[7];

      sprintf( tbuff, "%5.2f", ival);    /* store value without leading 0 */
      strcpy( buff, tbuff + 1);
      }
}

void format_dist_in_buff( char *buff, const double dist_in_au)
{
   if( dist_in_au < 0.)
      strcpy( buff, " <NEG!>");
   else if( use_au_only == true)
      show_dist_in_au( buff, dist_in_au);
   else
      {
      const double dist_in_km = dist_in_au * AU_IN_KM;
      const char *fmt;

                  /* for objects within a million km (about 2.5 times   */
                  /* the distance to the moon),  switch to km/m/cm/mm:  */
      if( dist_in_km < .0099)                 /* 0 to 9900 millimeters: */
         sprintf( buff, "%5.0fmm", dist_in_km * 1e+6);    /* " NNNNmm" */
      else if( dist_in_km < .099)             /* 990 to 9900 centimeters: */
         sprintf( buff, "%5.0fcm", dist_in_km * 1e+5);    /* " NNNNcm" */
      else if( dist_in_km < 99.)          /* 99 to 99000 meters: */
         sprintf( buff, "%6.0fm", dist_in_km * 1e+3);     /* " NNNNNm" */
      else if( dist_in_km < 999.)         /* 99.0 to 999.9 kilometers: */
         sprintf( buff, "%6.1fk", dist_in_km);            /* " NNN.Nk" */
      else if( dist_in_km < 999999.)      /* 999.9 to 999999 km: */
         sprintf( buff, "%7.0f", dist_in_km);
      else if( dist_in_au > 9999.999)
         {
         double dist_in_light_years =
                               dist_in_au * AU_IN_KM / LIGHT_YEAR_IN_KM;

         if( dist_in_light_years > 9999.9)
            {
            int idx = 0;

            dist_in_light_years /= 1000;
            for( idx = 0; si_prefixes[idx] && dist_in_light_years > 999.; idx++)
               dist_in_light_years /= 1000;
            if( !si_prefixes[idx])   /* can't represent this even in our */
               strcpy( buff, " <HUGE>");     /* largest made-up units */
            else
               {
               if( dist_in_light_years < 9.9)
                  sprintf( buff, "%4.1fxLY", dist_in_light_years);
               else
                  sprintf( buff, "%4.0fxLY", dist_in_light_years);
               buff[4] = si_prefixes[idx];
               }
            }
         else
            {
            if( dist_in_light_years > 99.999)  /* " 1234LY" */
               fmt = "%5.0fLY";
            else if( dist_in_light_years > 9.999)   /* " 12.3LY" */
               fmt = "%5.1fLY";
            else if( dist_in_light_years > .999)
               fmt = "%5.2fLY";           /* " 1.23LY" */
            else
               fmt = "%5.3fLY";           /* " .123LY" */
            sprintf( buff, fmt, dist_in_light_years);
            }
         }
      else
         show_dist_in_au( buff, dist_in_au);
      *buff = ' ';                /* remove leading zero for small amts */
      }
}


      /* Input velocity is in km/s.  If it's greater than about     */
      /* three times the speed of light,  we show it in units of c. */
      /* Of course,  this will never happen with real objects;  I   */
      /* added it for debugging purposes.                           */
static void format_velocity_in_buff( char *buff, double vel)
{
   const char *format;

   if( vel < 9.999 && vel > -9.999)
      format = "%7.3f";
   else if( vel < 99.999 && vel > -99.999)
      format = "%7.2f";
   else if( vel < 999.9 && vel > -999.9)
      format = "%7.1f";
   else if( vel < 999999. && vel > -999999.)
      format = "%7.0f";
   else         /* show velocity in terms of speed of light. */
      {
      vel /= SPEED_OF_LIGHT;
      if( vel < 99.999 && vel > -99.999)
         format = "%6.1fc";
      else if( vel < 999999. && vel > -999999.)
         format = "%6.0fc";
      else        /* we give up;  it's too fast */
         format =  " !!!!!!";
      }
   sprintf( buff, format, vel);
}

/* Rob Matson asked about having the program produce ECF (Earth-Centered
Fixed) coordinates,  in geometric lat/lon/altitude form.  I just hacked
it in,  then commented it out.  I'd have removed it,  but I'm thinking
this might be a useful option someday.  */

#ifdef ROB_MATSON_TEST_CODE
int find_lat_lon_alt( const double jd, const double *ivect,  /* collide.cpp */
                                  double *lat_lon_alt, const bool geometric);
#endif

/* 'get_step_size' parses input text to get a step size in days,  so that */
/* '4h' becomes .16667 days,  '30m' becomes 1/48 day,  and '10s' becomes  */
/* 10/(24*60*60) days.  The units (days, hours, minutes,  or seconds) are */
/* returned in 'step_units' if the input pointer is non-NULL.  The number */
/* of digits necessary is returned in 'step_digits' if that pointer is    */
/* non-NULL.  Both are used to ensure correct time format in ephemeris    */
/* output;  that is,  if the step size is (say) .05d,  the output times   */
/* ought to be in a format such as '2009 Mar 8.34', two places in days.   */

double get_step_size( const char *stepsize, char *step_units, int *step_digits)
{
   double step = 0.;
   char units = 'd';

   if( sscanf( stepsize, "%lf%c", &step, &units) >= 1)
      if( step)
         {
         if( step_digits)
            {
            const char *tptr = strchr( stepsize, '.');

            *step_digits = 0;
            if( tptr)
               {
               tptr++;
               while( isdigit( *tptr++))
                  (*step_digits)++;
               }
#ifdef OBSOLETE_METHOD
            double tval = fabs( step);

            for( *step_digits = 0; tval < .999; (*step_digits)++)
               tval *= 10.;
#endif
            }
         units = tolower( units);
         if( step_units)
            *step_units = units;
         switch( units)
            {
            case 'd':
               break;
            case 'h':
               step /= hours_per_day;
               break;
            case 'm':
               step /= minutes_per_day;
               break;
            case 's':
               step /= seconds_per_day;
               break;
            case 'w':
               step *= 7.;
               break;
            case 'y':
               step *= 365.25;
               break;
            }
         }
   return( step);
}

int find_precovery_plates( const char *filename, const double *orbit,
                           double epoch_jd)
{
   FILE *ofile, *ifile = fopen( "sky_cov.txt", "rb");
   double orbi[6];
   char buff[100];

   if( !ifile)
      return( -2);
   ofile = fopen( filename, "w");
   if( !ofile)
      {
      fclose( ifile);
      return( -1);
      }
   setvbuf( ofile, NULL, _IONBF, 0);
   memcpy( orbi, orbit, 6 * sizeof( double));
   while( fgets_trimmed( buff, sizeof( buff), ifile))
      {
      char tbuff[80];
      int line_no = 0;
      FILE *coverage_file = fopen( buff + 8, "rb");
      const long jd = dmy_to_day( 1, 1, atol( buff) / 1000, CALENDAR_GREGORIAN)
                                + atol( buff) % 1000;
      const double curr_jd = (double)jd + .5;
      double obs_posn[3], topo[3];
      double obj_ra, obj_dec;
      int i;

      integrate_orbit( orbi, epoch_jd, curr_jd);
      epoch_jd = curr_jd;
      compute_observer_loc( curr_jd, 3, 0., 0., 0., obs_posn);
      for( i = 0; i < 3; i++)
         topo[i] = orbi[i] - obs_posn[i];
      ecliptic_to_equatorial( topo);
      obj_ra = atan2( topo[1], topo[0]);
      obj_dec = asin( topo[2] / vector3_length( topo));

//    fprintf( ofile, "Looking at %s\n", buff);
//    fprintf( ofile, "JD %f; %f %f\n", curr_jd, obj_ra * 180. / PI,
//             obj_dec * 180. / PI);
      if( coverage_file)
         {
         while( fgets( tbuff, sizeof( tbuff), coverage_file))
            {
            double ra_min, ra_max, dec_min, dec_max;

            line_no++;
            ra_min = ra_max = atof( tbuff);
            dec_min = dec_max = atof( tbuff + 10);
            for( i = 1; i < 4; i++)
               {
               double ra = atof( tbuff + i * 18 + 1);
               double dec = atof( tbuff + i * 18 + 10);

               while( ra - ra_min > PI)
                  ra -= PI + PI;
               while( ra - ra_max < -PI)
                  ra += PI + PI;
               if( ra_min > ra)
                  ra_min = ra;
               if( ra_max < ra)
                  ra_max = ra;
               if( dec_min > dec)
                  dec_min = dec;
               if( dec_max < dec)
                  dec_max = dec;
               }
            while( obj_ra - ra_min > PI)
               obj_ra -= PI + PI;
            while( obj_ra - ra_min < -PI)
               obj_ra += PI + PI;
            if (obj_ra > ra_min && obj_ra < ra_max && obj_dec > dec_min
                            && obj_dec < dec_max)
               fprintf( ofile, "%4d %s\n", line_no, buff + 8);
            }
         fclose( coverage_file);
         }
      }
   fclose( ifile);
   fclose( ofile);
   return( 0);
}

/* In the following,  I'm assuming an object with H=0 and albedo=100% to
have a diameter of 1300 km.  Return value is in meters. */

double diameter_from_abs_mag( const double abs_mag,
                                     const double optical_albedo)
{
   return( 1300. * 1000. * pow( .1, abs_mag / 5.) / sqrt( optical_albedo));
}

#define RADAR_DATA struct radar_data

RADAR_DATA
   {
   double power_in_watts;
   double system_temp_deg_k;
   double gain, radar_constant;
   double altitude_limit;
   };

static inline int get_radar_data( const char *mpc_code, RADAR_DATA *rdata)
{
   char tbuff[20];
   const char *tptr;
   int rval = -1;

   memset( rdata, 0, sizeof( RADAR_DATA));
   sprintf( tbuff, "RADAR_%.3s", mpc_code);
   tptr = get_environment_ptr( tbuff);
   if( tptr)
      if( sscanf( tptr, "%lf,%lf,%lf,%lf,%lf",
            &rdata->power_in_watts, &rdata->system_temp_deg_k,
            &rdata->gain, &rdata->altitude_limit,
            &rdata->radar_constant) == 5)
         rval = 0;
   rdata->altitude_limit *= PI / 180.;
   return( rval);
}

/* In computing radar SNR,  we need to either measure the rotation period
(if we're lucky,  and somebody does a light curve) or take an educated guess
at it,  as a function of absolute magnitude.  In general,  smaller rocks
spin faster than bigger rocks.

   I asked about this,  and Mike Nolan replied : "I tend to use 4h for
larger than H=21, 1 for H=21-25, and .25 for H > 25. We don't really know
the distribution for the small stuff, as the biases are pretty big." To
which Lance Benner replied : "We've been adopting a somewhat more
conservative value of P = 2.1 h for objects with  H < 22.   This is based
on the observed distribution of NEA rotation periods (although there are a
couple of exceptions).  For smaller objects, a similar cutoff doesn't seem
to exist, but rapid rotators are relatively common, so we adopt P = 0.5h."

   In the following,  I went with a three-hour period for H<=21 and a .3
hour period for H>=25,  linearly interpolating for 21 < H < 25.

   Fortunately,  SNR/day in the formula below depends on the square root of
the period.  If we're out by a factor of two on our guess,  it'll mess up the
SNR/day by a factor of the square root of two.  I'd guess that errors due to
under- or over-estimating the albedo or H probably cause much more trouble.
*/

static double guessed_rotation_period_in_hours( const double abs_mag)
{
   double rval;
   const double big_limit = 21., big_period = 3.;  /* H=21 rocks spin in 3h */
   const double small_limit = 25., small_period = 0.3;
                           /* little H=25 rocks spin in a mere 18 minutes */

   if( abs_mag < big_limit)
      rval = big_period;
   else if( abs_mag < small_limit)
      rval = small_period +
               (big_period - small_period) * (small_limit - abs_mag) / (small_limit - big_limit);
   else        /* small,  fast-spinning rocks  */
      rval = small_period;
   return( rval);
}

/*   In a private communication,  Lance Benner wrote:

    "...The signal-to-noise ratio is proportional to:

[(Ptx)(radar albedo)(D^3/2)(P^1/2)(t^1/2)]  <-- numerator
------------------------------------------
[(r^4)*(Tsys)]                              <-- denominator

where Ptx is the transmitter power, the radar albedo is the radar cross section
divided by the projected area of the object, D is the diameter, P is the
rotation period, t is the integration time, r is the distance from the
telescope, and Tsys is the system temperature."

   Implicitly,  this is also all multiplied by a gain factor,  and a
constant known in this code as 'radar_constant'.

   He also mentioned that Arecibo operates at about 500 kW,  Tsys = 25 K,
gain = 10 K/Jy;  Goldstone at 450 kW,  Tsys = 15, gain = 0.94.  These
values are enshrined in the RADAR_251 and RADAR_253 lines in 'environ.dat',
along with the 'radar_constant' values.  I got those essentially by guessing,
then scaling them until they matched values in radar planning documents.

   See also Lance's comments at

http://tech.groups.yahoo.com/group/mpml/message/28688

   Lance also noted that there's also a more subtle dependence due to the subradar
latitude (you get a better echo if the pole is pointing straight at the radar).

   Jean-Luc Margot's page:

http://www2.ess.ucla.edu/~jlm/research/NEAs/intro.html

   has a graphic showing this dependence.  A 20-meter object with a four-hour
period and albedo=.1,  at a distance of .1 AU,  has SNR/day = 1.

   For Goldstone,  the SNR is about 1/20 of this (from JLM's page;  I think
this assumes Arecibo going at 900 kW.)  The same page shows a dropoff in SNR
due to declination:  the above formula holds at dec=+18,  but SNR drops to
zero at dec=-1 or dec=37.  In reality,  it should be a drop in _altitude_ :
i.e.,  Arecibo sees "perfectly" at the zenith,  and degrades as you drop from
there.  Mike Nolan tells me (private e-mail communication) that the width is
about 19.5 degrees,  so that Arecibo can "see"  down to about altitude = 70.5
degrees.   The drop factor isn't given (and is probably empirically derived),
but it looks a lot like

SNR( alt) = SNR( zenith) * ((90 - alt)/19.5) ^ .25,     70.5 < alt < 90

   Or something like that,  perhaps with a different exponent:  a function
equal to one at alt=90 degrees and zero at alt=70.5 degrees,  with roughly
the right behavior in between. */

const double optical_albedo = .1;

static double radar_snr_per_day( const RADAR_DATA *rdata,
                     const double abs_mag,
                     const double radar_albedo,
                     const double dist)
{
   const double rotation_period = guessed_rotation_period_in_hours( abs_mag);
   const double diameter_in_meters =
                 diameter_from_abs_mag( abs_mag, optical_albedo);
   double snr_per_day =
            rdata->radar_constant * radar_albedo
            * sqrt( rotation_period * diameter_in_meters)
                        * diameter_in_meters / pow( dist, 4.);

   snr_per_day *= rdata->power_in_watts * rdata->gain;
   snr_per_day /= rdata->system_temp_deg_k;
   return( snr_per_day);
}


/*    Explanation of how 'shadow_check' determines if a given point is
in the earth's shadow :

---_
    \                     __
     \                   /  \        . <- Obj_posn
 .    |                 |    |
      |                  \__/
     /
____/

radius Rsun             radius Rearth

   To determine if an object is in the earth's shadow:  in the above
diagram,  with the sun at the origin (0,0) and the earth along the x-axis
at (earth_r, 0),  with earth_r = length of earth_loc vector,  the object
is at

x = (earth_loc . obj_posn) / earth_r

   If x < earth_r,  we're closer to the sun than the earth is,  and are
definitely not in the shadow.  If x > earth_r,  we can compute the radius
of the earth's (umbral) shadow at that point as

shadow_radius = Rearth - (x - earth_r) * (Rsun - Rearth) / earth_r

   If shadow_radius < 0,  we're so far out from the sun that we can't
possibly be in the umbra.  (Which is to say,  we're about four times
further out than the moon,  at the point where the earth could just
barely block the sun.)  But if x > earth_r and shadow_radius > 0,  we
have to compute :

off-axis vector = obj_posn - x * earth_loc / earth_r
y = |off-axis vector|

   And if y < shadow_radius,  we're in the umbra.
*/



static bool shadow_check( const double *earth_loc, const double *obs_posn)
{
   const double earth_r = vector3_length( earth_loc);
   const double x = dot_product( earth_loc, obs_posn) / earth_r;
   bool is_in_shadow = false;

   if( x > earth_r)
      {
      const double SUN_RADIUS_IN_AU = 696000. / AU_IN_KM;
      const double shadow_radius = EARTH_MAJOR_AXIS_IN_AU
              - (x - earth_r) * (SUN_RADIUS_IN_AU - EARTH_MAJOR_AXIS_IN_AU) / earth_r;

      if( shadow_radius > 0)
         {
         double off_axis_vector[3], y;
         size_t i;

         for( i = 0; i < 3; i++)
            off_axis_vector[i] = obs_posn[i] - earth_loc[i] * x / earth_r;
         y = vector3_length( off_axis_vector);
         if( y < shadow_radius)
            is_in_shadow = true;
         }
      }
   return( is_in_shadow);
}

double vector_to_polar( double *lon, double *lat, const double *vector)
{
   const double r = vector3_length( vector);

   *lon = PI + atan2( -vector[1], -vector[0]);
   *lat =  asin( vector[2] / r);
   return( r);
}

static void format_motion( char *buff, const double motion)
{
   const char *motion_format;
   const double fabs_motion = fabs( motion);

   if( fabs_motion > 999999.)
      motion_format = "------";
   else if( fabs_motion > 999.)
      motion_format = "%6.0f";
   else if( fabs_motion > 99.9)
      motion_format = "%6.1f";
   else
      motion_format = "%6.2f";
   sprintf( buff, motion_format, motion);
}

void **calloc_double_dimension_array( const size_t x, const size_t y,
                                    const size_t obj_size);

#ifdef ENABLE_SCATTERPLOTS

static inline char **make_text_scattergram( const double *x, const double *y,
         const unsigned n_pts,
         const int xsize, const int ysize, double range)
{
   int i;
   char **histo = (char **)calloc_double_dimension_array(
                                    ysize + 1, xsize + 10, 1);
   double scale = range / (double)ysize;
   int iscale = 1, spacing = 0, n_marks;

   for( i = 0; iscale < 1000000 && spacing < 3; i++)
      {                 /* iscale will run as 1, 2, 4, 5, 10, 20, 40, ... */
      if( i % 4 == 2)
         iscale += iscale / 4;
      else
         iscale += iscale;
      spacing = iscale / (int)( scale + 1.);
      }
   range = (double)ysize * (double)iscale / (double)spacing;

   for( i = 0; i <= ysize; i++)
      memset( histo[i], ' ', xsize);

   for( i = 0; i < (int)n_pts; i++)
      {        /* RA runs "backward" on x-axis */
      const int ix = (xsize - (int)floor( x[i] * (double)xsize / range)) / 2;
      const int iy = (ysize + (int)floor( y[i] * (double)ysize / range)) / 2;

      if( ix >= 0 && ix < xsize && iy > 0 && iy < ysize)
         {
         if( histo[iy][ix] == ' ')
            histo[iy][ix] = '1';
         else if( histo[iy][ix] < 'Z')
            histo[iy][ix]++;
         }
      }
   n_marks = (ysize / 2) / spacing;
   for( i = -n_marks; i <= n_marks; i++)
      {
      char text[20];
      int j, num = i * iscale;
      int yloc = ysize / 2 + i * spacing, xloc;

      for( j = -n_marks; j <= n_marks; j++)
         histo[yloc][xsize / 2 + 2 * j * spacing] = '+';
      if( !num)
         strcpy( text, "-0");
      else if( abs( num) < 1000)
         sprintf( text, "-%+d", num);
      else if( !(num % 1000))
         sprintf( text, "-%+dK", num / 1000);
      else
         strcpy( text, "-");
      strcat( histo[yloc], text);
      xloc = yloc * 2 - (int)strlen( text + 1) / 2;
      if( xloc >= 0)
         memcpy( histo[0] + xloc, text + 1, strlen( text + 1));
      }
   histo[ysize / 2][xsize / 2] = '*';       /* mark center */
   strcpy( histo[ysize], histo[0]);         /* copy top line to bottom */
   return( histo);
}

   /* In displaying an SR scatterplot,  we might want to size it */
   /* such that,  say,  17 of 20 points appear,  with the three  */
   /* largest outliers omitted.  'find_cutoff_point' finds the   */
   /* range required to accomplish this.  It first finds the     */
   /* range required to accommodate _all_ the points,  then      */
   /* gradually reduces this until it's too small.  Then it goes */
   /* back to the size that _did_ accommodate at least that many */
   /* data points,  and returns that value.                      */

static inline double find_cutoff_point( const double *x, const double *y,
         const unsigned n_pts, const unsigned target_n_inside)
{
   double lim = 0.;
   unsigned i, n_found_inside = n_pts;

   for( i = 0; i < n_pts; i++)
      {
      if( lim < fabs( x[i]))
         lim = fabs( x[i]);
      if( lim < fabs( y[i]))
         lim = fabs( y[i]);
      }
   while( n_found_inside > target_n_inside)
      {
      lim /= 1.1;
      n_found_inside = 0;
      for( i = 0; i < n_pts; i++)
         if( fabs( x[i]) < lim && fabs( y[i]) < lim)
            n_found_inside++;
      }
   return( lim * 1.1);
}

#endif        /* #ifdef ENABLE_SCATTERPLOTS */

inline void calc_sr_dist_and_posn_ang( const DPT *ra_decs, const unsigned n_objects,
                     double *dist, double *posn_ang)
{
   unsigned i;
   double mean_x = 0., mean_y = 0.;
   const double ra0 = ra_decs[0].x, dec0 = ra_decs[0].y;
   double *x, *y;
   double sum_x2 = 0., sum_y2 = 0., sum_xy = 0.;
   double b, c, discrim, z1, z2;
   const double radians_to_arcsecs = 180. * 3600. / PI;

   x = (double *)calloc( n_objects * 2, sizeof( double));
   if( !x)
      return;
   y = x + n_objects;
   for( i = 1; i < n_objects; i++)
      {
      double dx = centralize_ang( ra_decs[i].x - ra0);
      double dy =                 ra_decs[i].y - dec0;

      if( dx > PI)
         dx -= PI + PI;
      dx *= cos( dec0) * radians_to_arcsecs;
      dy *=              radians_to_arcsecs;
      x[i] = dx;
      y[i] = dy;
      mean_x += dx;
      mean_y += dy;
      }
   mean_x /= (double)n_objects;
   mean_y /= (double)n_objects;
   for( i = 0; i < n_objects; i++)
      {
      const double dx = x[i] - mean_x;
      const double dy = y[i] - mean_y;

      sum_x2 += dx * dx;
      sum_xy += dx * dy;
      sum_y2 += dy * dy;
//    debug_printf( "  %u: %f %f (%f %f)\n", i,
//                   ra_decs[i].x * 180. / PI, ra_decs[i].y * 180. / PI,
//                   dx, dy);
      }                         /* We now have a covariance matrix: */
   sum_x2 /= (double)n_objects;        /*   / sum_x2 sum_xy \        */
   sum_xy /= (double)n_objects;        /*  (                 )       */
   sum_y2 /= (double)n_objects;        /*   \ sum_xy sum_y2 /        */
            /* The eigenvals of this are the zeroes z1, z2 of :      */
            /* z^2 - (sum_x2 + sum_y2)z + sum_x2*sum_y2 - (sum_xy)^2 */
   b = -(sum_x2 + sum_y2);
   c = sum_x2 * sum_y2 - sum_xy * sum_xy;
   discrim = b * b - 4. * c;
   assert( discrim >= 0.);
   z1 = (-b + sqrt( discrim)) * .5;
   z2 = c / z1;
   *dist = sqrt( z1);
   assert( z1 > z2);    /* z1 should be the _major_ axis */
   *posn_ang = atan2( sum_xy, sum_x2 - z2);
#ifdef ENABLE_SCATTERPLOTS
// debug_printf( "Posn angles   : %f %f\n", *posn_ang * 180. / PI,
//          atan2( sum_xy, sum_x2 - z1) * 180. / PI);
// debug_printf( "Posn angles(2): %f %f\n",
//          atan2( sum_y2 - z1, sum_xy) * 180. / PI,
//          atan2( sum_y2 - z2, sum_xy) * 180. / PI);
   debug_printf( "Unc ellipse: %f x %f arcsec\n",
               sqrt( z1), sqrt( z2));
      {
      const unsigned ysize = 30, xsize = ysize * 2;
      char **histo = make_text_scattergram( x, y, n_objects, xsize, ysize,
                    find_cutoff_point( x, y, n_objects, n_objects * 9 / 10));

      for( i = 0; i <= ysize; i++)
         debug_printf( "%s\n", histo[ysize - i]);
      free( histo);
      }
#endif
   *dist /= radians_to_arcsecs;
   free( x);
}

double ephemeris_mag_limit = 22.;

int ephemeris_in_a_file( const char *filename, const double *orbit,
         OBSERVE *obs, const int n_obs,
         const int planet_no,
         const double epoch_jd, const double jd_start, const char *stepsize,
         const double lon,
         const double rho_cos_phi, const double rho_sin_phi,
         const int n_steps, const char *note_text,
         const int options, const unsigned n_objects)
{
   double *orbits_at_epoch, step;
   DPT *stored_ra_decs;
   double prev_ephem_t = epoch_jd, prev_radial_vel = 0.;
   int i, hh_mm, n_step_digits;
   unsigned date_format;
   const int ephem_type = (options & 7);
   FILE *ofile;
   const bool computer_friendly = ((options & OPTION_COMPUTER_FRIENDLY) ? true : false);
   char step_units;
   const char *timescale = get_environment_ptr( "TT_EPHEMERIS");
   const char *override_date_format = get_environment_ptr( "DATE_FORMAT");
   const bool show_topocentric_data =
           ( rho_cos_phi && rho_sin_phi && ephem_type == OPTION_OBSERVABLES);
   const bool show_alt_az = ((options & OPTION_ALT_AZ_OUTPUT)
                        && show_topocentric_data);
   const bool show_visibility = ((options & OPTION_VISIBILITY)
                        && show_topocentric_data);
   const bool show_uncertainties = ((options & OPTION_SHOW_SIGMAS)
                        && n_objects > 1 && ephem_type == OPTION_OBSERVABLES);
   double abs_mag = calc_absolute_magnitude( obs, n_obs);
   double unused_ht_in_meters;
   DPT latlon;
   bool last_line_shown = true;
   RADAR_DATA rdata;
   bool show_radar_data = (get_radar_data( note_text + 1, &rdata) == 0);
   const double planet_radius_in_au =
          planet_radius_in_meters( planet_no) / AU_IN_METERS;

   step = get_step_size( stepsize, &step_units, &n_step_digits);
   if( !step)
      return( -2);
   ofile = fopen_ext( filename, "fcw");
   if( !ofile)
      return( -1);
   if( !abs_mag)
      abs_mag = atof( get_environment_ptr( "ABS_MAG"));
   orbits_at_epoch = (double *)calloc( n_objects, 8 * sizeof( double));
   memcpy( orbits_at_epoch, orbit, n_objects * 6 * sizeof( double));
   stored_ra_decs = (DPT *)( orbits_at_epoch + 6 * n_objects);
   setvbuf( ofile, NULL, _IONBF, 0);
   switch( step_units)
      {
      case 'd':
         hh_mm = 0;
         date_format = FULL_CTIME_DATE_ONLY;
         break;
      case 'h':
         hh_mm = 1;
         date_format = FULL_CTIME_FORMAT_HH;
         break;
      case 'm':
         hh_mm = 2;
         date_format = FULL_CTIME_FORMAT_HH_MM;
         break;
      case 's':
      default:
         hh_mm = 3;
         date_format = FULL_CTIME_FORMAT_SECONDS;
         break;
      }
   date_format |= FULL_CTIME_YEAR_FIRST | FULL_CTIME_MONTH_DAY
            | FULL_CTIME_MONTHS_AS_DIGITS | FULL_CTIME_LEADING_ZEROES
            | CALENDAR_JULIAN_GREGORIAN;
   date_format |= (n_step_digits << 4);
                     /* For ease of automated processing,  people may want */
   if( *override_date_format)     /* the time in some consistent format... */
      sscanf( override_date_format, "%x", &date_format);
   if( ephem_type == OPTION_STATE_VECTOR_OUTPUT ||
       ephem_type == OPTION_POSITION_OUTPUT ||
       ephem_type == OPTION_MPCORB_OUTPUT ||
       ephem_type == OPTION_8_LINE_OUTPUT)
      {
      timescale = "y";        /* force TT output */
      fprintf( ofile, "%.5f %f %d\n", jd_start, step, n_steps);
      }
   else if( ephem_type != OPTION_CLOSE_APPROACHES)
      {
      char hr_min_text[80];
      const char *pre_texts[4] = { "", " HH", " HH:MM", " HH:MM:SS" };

      strcpy( hr_min_text, pre_texts[hh_mm]);
      if( n_step_digits)
         {
         strcat( hr_min_text, ".");
         for( i = n_step_digits; i; i--)
            {
            char tbuff[2];

            tbuff[0] = step_units;
            tbuff[1] = '\0';
            strcat( hr_min_text, tbuff);
            }
         }
      if( note_text)
         fprintf( ofile, "#%s\n", note_text);
      if( !computer_friendly)
         {
         if( show_radar_data)
            {
            fprintf( ofile,
                  "Assumes power=%.2f kW, Tsys=%.1f deg K, gain %.2f K/Jy\n",
                  rdata.power_in_watts / 1000., rdata.system_temp_deg_k,
                  rdata.gain);
            fprintf( ofile,
               "Assumed rotation period = %.2f hours, diameter %.1f meters\n",
                           guessed_rotation_period_in_hours( abs_mag),
                           diameter_from_abs_mag( abs_mag, optical_albedo));
            }
         fprintf( ofile, "Date %s%s   RA              ",
                        (*timescale ? "(TT)"  : "(UTC)"), hr_min_text);
         fprintf( ofile, "Dec         delta   r     elong ");
         if( show_visibility)
            fprintf( ofile, "SM ");
         if( options & OPTION_PHASE_ANGLE_OUTPUT)
            fprintf( ofile, " ph_ang  ");
         if( options & OPTION_PHASE_ANGLE_BISECTOR)
            fprintf( ofile, " ph_ang_bisector  ");
         if( options & OPTION_HELIO_ECLIPTIC)
            fprintf( ofile, " helio ecliptic   ");
         if( options & OPTION_TOPO_ECLIPTIC)
            fprintf( ofile, " topo ecliptic    ");
         if( abs_mag)
            fprintf( ofile, " mag");

         if( options & OPTION_LUNAR_ELONGATION)
            fprintf( ofile, "  LuElo");
         if( options & OPTION_MOTION_OUTPUT)
            fprintf( ofile, (options & OPTION_SEPARATE_MOTIONS) ? "  RA '/hr dec " : "  '/hr    PA  ");
         if( show_alt_az)
            fprintf( ofile, " alt  az");
         if( options & OPTION_RADIAL_VEL_OUTPUT)
            fprintf( ofile, "  rvel ");
         if( show_radar_data)
            fprintf( ofile, "  SNR");
#ifdef ROB_MATSON_TEST_CODE
         if( options & OPTION_GROUND_TRACK)
            fprintf( ofile, "  lon      lat      alt (km) ");
#endif
         if( options & OPTION_SPACE_VEL_OUTPUT)
            fprintf( ofile, "  svel ");
         if( show_uncertainties)
            fprintf( ofile, " \" sig PA");
         fprintf( ofile, "\n");

         for( i = 0; hr_min_text[i]; i++)
            if( hr_min_text[i] != ' ')
               hr_min_text[i] = '-';
         fprintf( ofile, "---- -- --%s  ------------   ",  hr_min_text);
         fprintf( ofile, "------------  ------ ------ ----- ");
         if( show_visibility)
            fprintf( ofile, "-- ");
         if( options & OPTION_PHASE_ANGLE_OUTPUT)
            fprintf( ofile, " ------  ");
         if( options & OPTION_PHASE_ANGLE_BISECTOR)
            fprintf( ofile, " ---------------  ");
         if( options & OPTION_HELIO_ECLIPTIC)
            fprintf( ofile, " ---------------  ");
         if( options & OPTION_TOPO_ECLIPTIC)
            fprintf( ofile, " ---------------  ");
         if( abs_mag)
            fprintf( ofile, " ---");
         if( options & OPTION_LUNAR_ELONGATION)
            fprintf( ofile, "  -----");
         if( options & OPTION_MOTION_OUTPUT)
            fprintf( ofile, " ------ ------");
         if( show_alt_az)
            fprintf( ofile, " --- ---");
         if( options & OPTION_RADIAL_VEL_OUTPUT)
            fprintf( ofile, "  -----");
         if( show_radar_data)
            fprintf( ofile, " ----");
#ifdef ROB_MATSON_TEST_CODE
         if( options & OPTION_GROUND_TRACK)
            fprintf( ofile, " -------- -------- ----------");
#endif
         if( options & OPTION_SPACE_VEL_OUTPUT)
            fprintf( ofile, "  -----");
         if( show_uncertainties)
            fprintf( ofile, " ---- ---");
         fprintf( ofile, "\n");
         }
      }

   latlon.x = lon;
   parallax_to_lat_alt(
            rho_cos_phi / planet_radius_in_au,
            rho_sin_phi / planet_radius_in_au, &latlon.y,
                                &unused_ht_in_meters, planet_no);

   for( i = 0; i < n_steps; i++)
      {
      unsigned obj_n;
      bool show_this_line = true;
      double ephemeris_t, utc;
      double obs_posn[3], obs_vel[3];
      double obs_posn_equatorial[3];
      double geo_posn[3], geo_vel[3];
      char buff[440];
      double curr_jd = jd_start + (double)i * step, delta_t;

      if( options & OPTION_ROUND_TO_NEAREST_STEP)
         curr_jd = floor( (curr_jd - .5) / step + .5) * step + .5;
      delta_t = td_minus_utc( curr_jd) / seconds_per_day;
      if( *timescale)                     /* we want a TT ephemeris */
         {
         ephemeris_t = curr_jd;
         utc = curr_jd - delta_t;
         }
      else                                /* "standard" UTC ephemeris */
         {
         ephemeris_t = curr_jd + delta_t;
         utc = curr_jd;
         }
      compute_observer_loc( ephemeris_t, planet_no, rho_cos_phi, rho_sin_phi,
                              lon, obs_posn);
      compute_observer_vel( ephemeris_t, planet_no, rho_cos_phi, rho_sin_phi,
                              lon, obs_vel);
      compute_observer_loc( ephemeris_t, planet_no, 0., 0., 0., geo_posn);
      compute_observer_vel( ephemeris_t, planet_no, 0., 0., 0., geo_vel);
                /* we need the observer position in equatorial coords too: */
      memcpy( obs_posn_equatorial, obs_posn, 3 * sizeof( double));
      ecliptic_to_equatorial( obs_posn_equatorial);
      for( obj_n = 0; obj_n < n_objects && (!obj_n || show_uncertainties); obj_n++)
         {
         double *orbi = orbits_at_epoch + obj_n * 6;
         double radial_vel, v_dot_r;
         double topo[3], topo_vel[3], geo[3], r;
#ifdef SOLRAD_11B_HACK
         double gvel[3];
#endif
         double topo_ecliptic[3];
         double orbi_after_light_lag[3];
         OBSERVE temp_obs;
         int j;

         integrate_orbit( orbi, prev_ephem_t, ephemeris_t);
         for( j = 0; j < 3; j++)
            {
            topo[j] = orbi[j] - obs_posn[j];
            geo[j] = orbi[j] - geo_posn[j];
            topo_vel[j] = orbi[j + 3] - obs_vel[j];
#ifdef SOLRAD_11B_HACK
            gvel[j] = orbi[j + 3] - geo_vel[j];
#endif
            }
#ifdef SOLRAD_11B_HACK
         for( j = 0; j < 3; j++)       /* reflect through the geocenter  */
            {                             /* Note that geo & gvel are */
            topo[j] -= geo[j] * 2.;       /* simply negated.             */
            geo[j] -= geo[j] * 2.;
            topo_vel[j] -= gvel[j] * 2.;
            gvel[j] -= gvel[j] * 2.;
            }
#endif
         r = vector3_length( topo);
                    /* for "ordinary ephemeris" (not state vectors or */
                    /* orbital elements),  include light-time lag:    */
         if( ephem_type == OPTION_OBSERVABLES)
            for( j = 0; j < 3; j++)
               {
               const double diff = -orbi[j + 3] * r / AU_PER_DAY;

               orbi_after_light_lag[j] = orbi[j] + diff;
               topo[j] += diff;
               geo[j] += diff;
               }

         memset( &temp_obs, 0, sizeof( OBSERVE));
         temp_obs.r = vector3_length( topo);
         for( j = 0; j < 3; j++)
            {
            temp_obs.vect[j] = topo[j] / r;
            temp_obs.obs_vel[j] = -topo_vel[j];
            }
         memcpy( temp_obs.obs_posn, obs_posn, 3 * sizeof( double));
         for( j = 0; j < 3; j++)
            temp_obs.obj_posn[j] = temp_obs.obs_posn[j] + topo[j];
                     /* rotate topo from ecliptic to equatorial */
         memcpy( topo_ecliptic, topo, 3 * sizeof( double));
         ecliptic_to_equatorial( topo);                           /* mpc_obs.cpp */
         ecliptic_to_equatorial( geo);
         ecliptic_to_equatorial( topo_vel);
         v_dot_r = 0.;
         for( j = 0; j < 3; j++)
            v_dot_r += topo[j] * topo_vel[j];
         r = vector3_length( topo);
         radial_vel = v_dot_r / r;

         if( ephem_type == OPTION_STATE_VECTOR_OUTPUT ||
               ephem_type == OPTION_POSITION_OUTPUT)
            {
            int ecliptic_coords = 0;      /* default to equatorial J2000 */
            double posn_mult = 1., vel_mult = 1.;     /* default to AU & AU/day */
            double tval = 1.;
            char format_text[20];

            snprintf( buff, sizeof( buff), "%.5f", curr_jd);
            sscanf( get_environment_ptr( "VECTOR_OPTS"), "%d,%lf,%lf",
                        &ecliptic_coords, &posn_mult, &tval);
            assert( tval);
            assert( posn_mult);
            vel_mult = posn_mult / tval;
            if( ecliptic_coords)
               {
               equatorial_to_ecliptic( topo);
               equatorial_to_ecliptic( topo_vel);
               }
            for( j = 10, tval = posn_mult; tval > 1.2; j--)
               tval /= 10.;
            snprintf( format_text, sizeof( format_text), "%%16.%df", j);
            for( j = 0; j < 3; j++)
               snprintf_append( buff, sizeof( buff), format_text,
                                 topo[j] * posn_mult);
            if( ephem_type == OPTION_STATE_VECTOR_OUTPUT)
               {
               strcat( buff, " ");
               for( j = 12, tval = vel_mult; tval > 1.2; j--)
                  tval /= 10.;
               snprintf( format_text, sizeof( format_text), "%%16.%df", j);
               for( j = 0; j < 3; j++)
                  snprintf_append( buff, sizeof( buff), format_text,
                                 topo_vel[j] * vel_mult);
               }
            }
         else if( ephem_type == OPTION_8_LINE_OUTPUT
                || ephem_type == OPTION_MPCORB_OUTPUT)
            {
            if( !obj_n)
               {
               FILE *ifile;
                              /* only show comment data on last line */
               const int output_options = // ELEM_OUT_HELIOCENTRIC_ONLY |
                            (i == n_steps - 1 ? 0 : ELEM_OUT_NO_COMMENT_DATA);

               write_out_elements_to_file( orbi, ephemeris_t, ephemeris_t,
                          obs, n_obs, "", 5, 0, output_options);
               ifile = fopen_ext( get_file_name( buff,
                        (ephem_type == OPTION_8_LINE_OUTPUT) ? elements_filename : "mpc_fmt.txt"),
                        "fcrb");
               while( fgets( buff, sizeof( buff), ifile))
                  fputs( buff, ofile);
               fclose( ifile);
               }
            *buff = '\0';
            show_this_line = last_line_shown = false;
            }
         else if( ephem_type == OPTION_CLOSE_APPROACHES)
            {
            if( (step > 0. && radial_vel >= 0. && prev_radial_vel < 0.)
               || (step < 0. && radial_vel <= 0. && prev_radial_vel > 0.))
               {
               double dt, v_squared = 0.;
               char date_buff[80];

               for( j = 0; j < 3; j++)
                  v_squared += topo_vel[j] * topo_vel[j];
               dt = -v_dot_r / v_squared;
               full_ctime( date_buff, curr_jd + dt,
                        FULL_CTIME_FORMAT_HH_MM
                      | FULL_CTIME_YEAR_FIRST | FULL_CTIME_MONTH_DAY
                      | FULL_CTIME_MONTHS_AS_DIGITS
                      | FULL_CTIME_LEADING_ZEROES);
               snprintf( buff, sizeof( buff), "Close approach at %s: ",
                                 date_buff);
               for( j = 0; j < 3; j++)
                  topo[j] += dt * topo_vel[j];
               format_dist_in_buff( buff + strlen( buff),
                        vector3_length( topo));
               fprintf( ofile, "%s\n", buff);
               }
            *buff = '\0';
            last_line_shown = show_this_line = false;
            }
         else if( ephem_type == OPTION_OBSERVABLES)
            {
            DPT ra_dec, alt_az[3];
            double cos_lunar_elong;
            double ra, dec, sec, earth_r = 0.;
            int hr, min, deg, dec_sign = '+';
            char ra_buff[80], dec_buff[80], date_buff[80];
            char r_buff[20], solar_r_buff[20];
            double cos_elong, solar_r;
            bool moon_more_than_half_lit = false;
            bool is_in_shadow = false;
            const double arcsec_to_radians = PI / (180. * 3600.);
//          const double span = 0.01512;
//          const double ra_offset = ((utc - 2457361.64226) * -15. / span - 2342.) * arcsec_to_radians;
//          const double dec_offset = ((utc - 2457361.64226) * -149. / span + 4563.) * arcsec_to_radians;
            const double ra_offset = atof( get_environment_ptr( "RA_OFFSET")) * arcsec_to_radians;
            const double dec_offset = atof( get_environment_ptr( "DEC_OFFSET")) * arcsec_to_radians;

            strcpy( buff, "Nothing to see here... move along... uninteresting... who cares?...");
            solar_r = vector3_length( orbi_after_light_lag);
            earth_r = vector3_length( obs_posn_equatorial);
            cos_elong = r * r + earth_r * earth_r - solar_r * solar_r;
            cos_elong /= 2. * earth_r * r;

            ra_dec.x = atan2( topo[1], topo[0]) + ra_offset;
            ra = ra_dec.x * 12. / PI;
            if( ra < 0.) ra += 24.;
            if( ra >= 24.) ra -= 24.;
            if( computer_friendly)
               snprintf( ra_buff, sizeof( ra_buff), "%9.5f", ra * 15.);
            else
               {
               hr  = (int)ra;
               min = (int)((ra - (double)hr) * 60.);
               sec =       (ra - (double)hr) * 3600. - 60. * (double)min;
               snprintf( ra_buff, sizeof( ra_buff), "%02d %02d %6.3f",
                                 hr, min, sec);
               if( ra_buff[6] == ' ')        /* leading zero */
                  ra_buff[6] = '0';
               }

            ra_dec.y = asin( topo[2] / r) + dec_offset;
            stored_ra_decs[obj_n] = ra_dec;
            if( n_objects > 1 && obj_n == n_objects - 1 && show_this_line)
               {
               double dist, posn_ang;
               unsigned dist_in_arcsec;
               char tbuff[10];
               int integer_posn_ang;

               if( n_objects == 2)
                  calc_dist_and_posn_ang( (const double *)&stored_ra_decs[0],
                                       (const double *)&ra_dec,
                                       &dist, &posn_ang);
               else
                  calc_sr_dist_and_posn_ang( stored_ra_decs, n_objects,
                                       &dist, &posn_ang);
               integer_posn_ang =
                           (int)( floor( -posn_ang * 180. / PI + .5)) % 180;
               while( integer_posn_ang < 0)
                  integer_posn_ang += 180;
               dist *= 180. * 3600. / PI;
               dist_in_arcsec = (unsigned)dist;
               if( computer_friendly)
                  sprintf( tbuff, "%6u", dist_in_arcsec);
               else if( dist_in_arcsec < 9)
                  sprintf( tbuff, "%4.1f", dist);
               else if( dist_in_arcsec < 10000)
                  sprintf( tbuff, "%4u", dist_in_arcsec);
               else if( dist_in_arcsec < 60000)
                  sprintf( tbuff, "%3u'", dist_in_arcsec / 60);
               else
                  sprintf( tbuff, "%3ud", dist_in_arcsec / 3600);
               fprintf( ofile, " %s %3d", tbuff, integer_posn_ang);
               }
            dec = ra_dec.y * 180. / PI;
            if( dec < 0.)
               {
               dec = -dec;
               dec_sign = '-';
               }
            if( !obj_n)
               for( j = 0; j < 3; j++)    /* compute alt/azzes of object (j=0), */
                  {                       /* sun (j=1), and moon (j=2)          */
                  DPT obj_ra_dec = ra_dec;

                  if( j)
                     {
                     int k;
                     double vect[3], earth_loc[3];

                     if( j == 1)    /* solar posn */
                        for( k = 0; k < 3; k++)
                           vect[k] = -obs_posn_equatorial[k];
                     else
                        {           /* we want the lunar posn */
                        earth_lunar_posn( ephemeris_t, earth_loc, vect);
                        for( k = 0; k < 3; k++)
                           vect[k] -= earth_loc[k];
                        moon_more_than_half_lit =
                              (dot_product( earth_loc, vect) > 0.);
                        ecliptic_to_equatorial( vect);   /* mpc_obs.cpp */
                        is_in_shadow = shadow_check( earth_loc, orbi_after_light_lag);
                        cos_lunar_elong = dot_product( vect, geo)
                                 / (vector3_length( vect) * vector3_length( geo));
                        }
                     vector_to_polar( &obj_ra_dec.x, &obj_ra_dec.y, vect);
                     }
                  obj_ra_dec.x = -obj_ra_dec.x;
                  full_ra_dec_to_alt_az( &obj_ra_dec, &alt_az[j], NULL, &latlon, utc, NULL);
                  alt_az[j].x = centralize_ang( alt_az[j].x + PI);
                  }
            if( computer_friendly)
               sprintf( dec_buff, "%9.5f", dec);
            else
               {
               deg = (int)dec;
               min = (int)( (dec - (double)deg) * 60.);
               sec =        (dec - (double)deg) * 3600. - (double)min * 60.;
               sprintf( dec_buff, "%c%02d %02d %5.2f", dec_sign, deg, min, sec);
               if( dec_buff[7] == ' ')        /* leading zero */
                  dec_buff[7] = '0';
               }

            if( computer_friendly)
               {
               sprintf( date_buff, "%13.5f", curr_jd);
               sprintf( r_buff, "%14.9f", r);
               sprintf( solar_r_buff, "%12.7f", solar_r);
               }
            else
               {
               full_ctime( date_buff, curr_jd, date_format);
                    /* the radar folks prefer the distance to be always in */
                    /* AU,  w/no switch to km for close approach objects: */
               use_au_only = show_radar_data;
               format_dist_in_buff( r_buff, r);
               use_au_only = false;
               format_dist_in_buff( solar_r_buff, solar_r);
               }
            sprintf( buff, "%s  %s   %s %s%s %5.1f",
                  date_buff, ra_buff, dec_buff, r_buff, solar_r_buff,
                  acose( cos_elong) * 180. / PI);

            if( show_visibility)
               {
               char tbuff[4];

               tbuff[0] = ' ';
               if( alt_az[1].y > 0.)
                  tbuff[1] = '*';         /* daylight */
               else if( alt_az[1].y > -6. * PI / 180.)
                  tbuff[1] = 'C';         /* civil twilight */
               else if( alt_az[1].y > -12. * PI / 180.)
                  tbuff[1] = 'N';         /* civil twilight */
               else if( alt_az[1].y > -18. * PI / 180.)
                  tbuff[1] = 'A';         /* civil twilight */
               else
                  tbuff[1] = ' ';         /* plain ol' night */
               if( alt_az[2].y > 0.)      /* moon's up */
                  tbuff[2] = (moon_more_than_half_lit ? 'M' : 'm');
               else
                  tbuff[2] = ' ';         /* moon's down */
               tbuff[3] = '\0';
               strcat( buff, tbuff);
               }
            if( !obj_n)
               {
               double phase_ang;
               double curr_mag = abs_mag + calc_obs_magnitude( obs->flags & OBS_IS_COMET,
                          solar_r, r, earth_r, &phase_ang);  /* elem_out.cpp */

               if( curr_mag > 999.)       /* avoid overflow for objects     */
                  curr_mag = 999.;        /* essentially at zero elongation */
               if( curr_mag > ephemeris_mag_limit)
                  show_this_line = false;

               if( options & OPTION_PHASE_ANGLE_OUTPUT)
                  snprintf_append( buff, sizeof( buff), " %8.4f", phase_ang * 180. / PI);

               if( options & OPTION_PHASE_ANGLE_BISECTOR)
                  {
                  double pab_vector[3], pab_lon, pab_lat;

                  for( j = 0; j < 3; j++)
                     pab_vector[j] = topo_ecliptic[j] / r
                                          + orbi_after_light_lag[j] / solar_r;
                  vector_to_polar( &pab_lon, &pab_lat, pab_vector);
                  snprintf_append( buff, sizeof( buff), " %8.4f %8.4f",
                                                     pab_lon * 180. / PI,
                                                     pab_lat * 180. / PI);
                  }

               if( options & OPTION_HELIO_ECLIPTIC)
                  {
                  double eclip_lon, eclip_lat;

                  vector_to_polar( &eclip_lon, &eclip_lat, orbi_after_light_lag);
                  snprintf_append( buff, sizeof( buff), " %8.4f %8.4f",
                                                     eclip_lon * 180. / PI,
                                                     eclip_lat * 180. / PI);
                  }

               if( options & OPTION_TOPO_ECLIPTIC)
                  {
                  double eclip_lon, eclip_lat;

                  vector_to_polar( &eclip_lon, &eclip_lat, topo_ecliptic);
                  snprintf_append( buff, sizeof( buff), " %8.4f %8.4f",
                                                     eclip_lon * 180. / PI,
                                                     eclip_lat * 180. / PI);
                  }

               if( abs_mag)           /* don't show a mag if you dunno how bright */
                  {                   /* the object really is! */
                  if( is_in_shadow)
                     strcat( buff, " Sha ");
                  else if( curr_mag < 99 && curr_mag > -9.9)
                     snprintf_append( buff, sizeof( buff), " %4.1f", curr_mag + .05);
                  else
                     snprintf_append( buff, sizeof( buff), " %3d ", (int)( curr_mag + .5));
                  if( phase_ang > PI * 2. / 3.)    /* over 120 degrees */
                     if( !(obs->flags & OBS_IS_COMET))   /* and isn't a comet */
                        {
                        char *endptr = buff + strlen( buff);

                        endptr[-1] = '?';      /* signal doubtful magnitude */
                        if( endptr[-2] == '.')
                           endptr[-2] = '?';
                        }
                  }
               }

            if( !obj_n && (options & OPTION_LUNAR_ELONGATION))
               sprintf( buff + strlen( buff), "%6.1f",
                                  acose( cos_lunar_elong) * 180. / PI);

            if( options & OPTION_MOTION_OUTPUT)
               {
               MOTION_DETAILS m;
               char *end_ptr = buff + strlen( buff);

               compute_observation_motion_details( &temp_obs, &m);
               strcat( buff, " ");
               if( options & OPTION_SEPARATE_MOTIONS)
                  {
                  format_motion( end_ptr + 1, m.ra_motion);
                  format_motion( end_ptr + 8, m.dec_motion);
                  }
               else
                  {
                  format_motion( end_ptr + 1, m.total_motion);
                  sprintf( end_ptr + 8, "%5.1f ",
                                  m.position_angle_of_motion);
                  }
               end_ptr[7] = ' ';
               }
            if( show_alt_az)
               {
                           /* FIX someday:  this only works if planet_no == 3, */
                           /* i.e.,  topocentric ephemerides */
               sprintf( buff + strlen( buff), " %c%02d %03d",
                                    (alt_az[0].y > 0. ? '+' : '-'),
                                    (int)( fabs( alt_az[0].y * 180. / PI) + .5),
                                    (int)( alt_az[0].x * 180. / PI + .5));
               }
            if( options & OPTION_RADIAL_VEL_OUTPUT)
               {
               char *end_ptr = buff + strlen( buff);
               const double rvel_in_km_per_sec =
                                        radial_vel * AU_IN_KM / seconds_per_day;

               if( computer_friendly)
                  sprintf( end_ptr, "%12.6f", rvel_in_km_per_sec);
               else
                  format_velocity_in_buff( end_ptr, rvel_in_km_per_sec);
               }
            if( show_radar_data)
               {
               if( alt_az[0].y < 0.)
                  strcat( buff, "  n/a");
               else
                  {
                  char *tptr = buff + strlen( buff);
                  const double radar_albedo = 0.1;
                  double snr = radar_snr_per_day( &rdata, abs_mag,
                                 radar_albedo, r);

                  *tptr = ' ';
                  show_packed_with_si_prefixes( tptr + 1, snr);
                  }
               }
#ifdef ROB_MATSON_TEST_CODE
            if( options & OPTION_GROUND_TRACK)
               {
               char *tptr = buff + strlen( buff);
               double lat_lon_alt[3];

               find_lat_lon_alt( ephemeris_t, geo, lat_lon_alt,
                        *get_environment_ptr( "GEOMETRIC_GROUND_TRACK") == '1');
               sprintf( tptr, "%9.4f %+08.4f %10.3f",
                     lat_lon_alt[0] * 180. / PI,
                     lat_lon_alt[1] * 180. / PI,
                     lat_lon_alt[2] * AU_IN_KM);
               tptr[29] = '\0';
               }
#endif
            if( options & OPTION_SPACE_VEL_OUTPUT)
               {
                        /* get 'full' velocity; cvt AU/day to km/sec: */
               const double total_vel =
                          vector3_length( topo_vel) * AU_IN_KM / seconds_per_day;

               format_velocity_in_buff( buff + strlen( buff), total_vel);
               }
            if( options & OPTION_SUPPRESS_UNOBSERVABLE)
               {
               if( show_radar_data)
                  {     /* for radar, 'observable' = obj above horizon */
                  show_this_line = (alt_az[0].y > rdata.altitude_limit);
                  }
               else if( show_topocentric_data && show_this_line)
                  {        /* "observable" = obj above horizon,  sun below it */
                  show_this_line = (alt_az[0].y > 0. && alt_az[1].y < 0.);
                  }
               }

            if( !show_this_line)
               {
               if( last_line_shown)
                  strcpy( buff, "................\n");
               else
                  *buff = '\0';
               }
            last_line_shown = show_this_line;
            }
         else        /* shouldn't happen */
            strcpy( buff, "DANGER!\n");
         if( !obj_n && (options & OPTION_MOIDS) && show_this_line)
            for( j = 1; j <= 8; j++)
               {
               double moid;
               ELEMENTS planet_elem, elem;
               const double GAUSS_K = .01720209895;  /* Gauss' grav const */
               const double SOLAR_GM = (GAUSS_K * GAUSS_K);

               elem.central_obj = 0;
               elem.gm = SOLAR_GM;
               elem.epoch = curr_jd;
               calc_classical_elements( &elem, orbi, curr_jd, 1);
               setup_planet_elem( &planet_elem, j, (curr_jd - J2000) / 36525.);
               moid = find_moid( &planet_elem, &elem, NULL);
               sprintf( buff + strlen( buff), "%8.4f", moid);
               }
         prev_radial_vel = radial_vel;
         if( !obj_n && *buff)
            fprintf( ofile, "%s", buff);
         }
      if( last_line_shown)
         fprintf( ofile, "\n");
      prev_ephem_t = ephemeris_t;
      }
   free( orbits_at_epoch);
   fclose( ofile);
   return( 0);
}

bool is_topocentric_mpc_code( const char *mpc_code)
{
   char buff[100];
   double rho_cos_phi, rho_sin_phi, lon;

   get_observer_data( mpc_code, buff, &lon, &rho_cos_phi, &rho_sin_phi);
   return( rho_cos_phi != 0. || rho_sin_phi != 0.);
}

int ephemeris_in_a_file_from_mpc_code( const char *filename,
         const double *orbit,
         OBSERVE *obs, const int n_obs,
         const double epoch_jd, const double jd_start, const char *stepsize,
         const int n_steps, const char *mpc_code,
         const int options, const unsigned n_objects)
{
   double rho_cos_phi, rho_sin_phi, lon;
   char note_text[100], buff[100];
   const int planet_no = get_observer_data( mpc_code, buff, &lon,
                                           &rho_cos_phi, &rho_sin_phi);

   snprintf( note_text, sizeof( note_text),
                    "(%s) %s", mpc_code, mpc_station_name( buff));
   return( ephemeris_in_a_file( filename, orbit, obs, n_obs, planet_no,
               epoch_jd, jd_start, stepsize, lon, rho_cos_phi, rho_sin_phi,
               n_steps, note_text, options, n_objects));
}

static int64_t ten_to_the_nth( int n)
{
   int64_t rval = 1;

   while( n--)
      rval *= 10;
   return( rval);
}

/* See comments for get_ra_dec() in mpc_obs.cpp for info on the meaning  */
/* of 'precision' in this function.                                      */

static void output_angle_to_buff( char *obuff, const double angle,
                               const int precision)
{
   int n_digits_to_show = 0;
   int64_t power_mul, fraction;
   size_t i;

   if( (precision >= 100 && precision <= 109) /* decimal quantity, dd.dd... */
        || ( precision >= 200 && precision <= 208)) /* decimal ddd.dd... */
      {
      const int two_digits = (precision <= 200);

      n_digits_to_show = precision % 100;
      power_mul = ten_to_the_nth( n_digits_to_show);
      fraction = (int64_t)( angle * (two_digits ? 1. : 15.) * (double)power_mul + .5);

      sprintf( obuff, (two_digits ? "%02d" : "%03d"),
                  (int)( fraction / power_mul));
      fraction %= power_mul;
      }
   else
      switch( precision)
         {
         case -1:       /* hh mm,  integer minutes */
         case -2:       /* hh mm.m,  tenths of minutes */
         case -3:       /* hh mm.mm,  hundredths of minutes */
         case -4:       /* hh mm.mmm,  milliminutes */
         case -5:       /* hh mm.mmmm */
         case -6:       /* hh mm.mmmmm */
         case -7:       /* hh mm.mmmmmm */
            {
            n_digits_to_show = -1 - precision;
            power_mul = ten_to_the_nth( n_digits_to_show);
            fraction = (int64_t)( angle * 60. * (double)power_mul + .5);
            sprintf( obuff, "%02d %02d", (int)( fraction / ((int64_t)60 * power_mul)),
                                         (int)( fraction / power_mul) % 60);
            fraction %= power_mul;
            }
            break;
         case 0:        /* hh mm ss,  integer seconds */
         case 1:        /* hh mm ss.s,  tenths of seconds */
         case 2:        /* hh mm ss.ss,  hundredths of seconds */
         case 3:        /* hh mm ss.sss,  thousands of seconds */
         case 307:      /* hhmmsss,  all packed together:  tenths */
         case 308:      /* hhmmssss,  all packed together: hundredths */
         case 309:      /* milliseconds (or milliarcseconds) */
         case 310:      /* formats 307-312 are the 'super-precise' formats */
         case 311:
         case 312:      /* microseconds (or microarcseconds) */
            {
            n_digits_to_show = precision % 306;
            power_mul = ten_to_the_nth( n_digits_to_show);
            fraction = (int64_t)( angle * 3600. * (double)power_mul + .5);
            sprintf( obuff, "%02d %02d %02d",
                     (int)( fraction / ((int64_t)3600 * power_mul)),
                     (int)( fraction / ((int64_t)60 * power_mul)) % 60,
                     (int)( fraction / power_mul) % 60);
            fraction %= power_mul;
            if( precision > 306)          /* remove spaces: */
               text_search_and_replace( obuff, " ", "");
            }
            break;
         default:                  /* try to show the angle,  but indicate */
            if( angle > -1000. && angle < 1000.)   /* the format is weird  */
               sprintf( obuff, "?%.5f", angle);
            else
               strcpy( obuff, "?");
            break;
         }
   if( n_digits_to_show)
      {
      char *tptr = obuff + strlen( obuff);
      char format[5];

      if( precision < 307 || precision > 312)   /* omit decimal point for */
         *tptr++ = '.';                         /* super-precise formats */
      sprintf( format, "%%0%dd", n_digits_to_show);
      sprintf( tptr, format, fraction);
      }
   for( i = strlen( obuff); i < 12; i++)
      obuff[i] = ' ';
   obuff[12] = '\0';
}

/* 'put_residual_into_text( )' expresses a residual,  from 0 to 180 degrees, */
/* such that the text starts with a space,  followed by four characters,   */
/* and a sign:  six bytes in all.  This can be in forms such as:           */
/*                                                                         */
/*  Err!+    (for values above 999 degrees)                                */
/*  179d-    (for values above 999 arcminutes = 16.6 degrees)              */
/*  314'+    (for values above 9999 arcsec but below 999 arcminutes)       */
/*  7821-    (for values below 9999 arcsec but above 99 arcsec)            */
/*  12.3+    (for values above one arcsec but below 99 arcsec)             */
/*  2.71-    (1" < value < 10",  if precise_residuals == 1)                */
/*   .87-    (for values under an arcsecond, usually)                      */
/*  .871-    (for values under an arcsecond, if precise_residuals == 1)    */
/*                                                                         */
/*     If precise_residuals == 2 ("super-precise" residuals) and the value */
/* is below a milliarcsecond,  some special formats such as '3.2u' or      */
/* '451u' ('u'=microarcseconds) or '4.5n' or '231n' (nanoarcsec),  or      */
/* even picoarcseconds,  are used.  (This because I was investigating      */
/* some roundoff/precision issues.  There would normally be no practical   */
/* reason to go to the picoarcsecond level of precision!)                  */

static void put_residual_into_text( char *text, const double resid,
                                 const int resid_format)
{
   double zval = fabs( resid);
   const int precise = resid_format
                & (RESIDUAL_FORMAT_OVERPRECISE | RESIDUAL_FORMAT_PRECISE);

   if( resid_format & RESIDUAL_FORMAT_COMPUTER_FRIENDLY)
      {                   /* resids in arcseconds at all times,  with */
      snprintf( text, 9, " %+.6f", resid);    /* some added precision */
      return;
      }
   if( zval > 999. * 3600.)      /* >999 degrees: error must have occurred */
      strcpy( text, " Err!");
   else if( zval > 59940.0)             /* >999': show integer degrees */
      sprintf( text, "%4.0fd", zval / 3600.);
   else if( zval > 9999.9)              /* 999' > x > 9999": show ###' arcmin */
      sprintf( text, "%4.0f'", zval / 60.);
   else if( zval > 99.9)
      sprintf( text, "%5.0f", zval);
   else if( zval > .99 && zval < 9.99 && precise)
      sprintf( text, "%5.2f", zval);
   else if( zval > .99)
      sprintf( text, "%5.1f", zval);
   else if( (resid_format & RESIDUAL_FORMAT_OVERPRECISE) && zval < .00999)
      {          /* 'high-precision' residuals */
      unsigned i;
      const char *lower_si_prefixes = " munpfazy ";

      for( i = 0; zval < 0.99 && i < 9; i++)
         zval *= 1000.;
      sprintf( text, (zval < 9.9 ? "%4.1f%c" : "%4.0f%c"),
                     zval, lower_si_prefixes[i]);
      }
   else
      {
      sprintf( text, (precise ? "%5.3f" : "%5.2f"), zval);
      text[precise ? 0 : 1] = ' ';
      }
   if( !atof( text))
      text[5] = ' ';
   else
      text[5] = (resid > 0. ? '+' : '-');
   text[6] = '\0';
}

static void show_dd_hh_mm_ss_point_sss( char *text,
                          const double day, int precision)
{
   const int64_t milliseconds = (int64_t)( day * seconds_per_day * 1000. + .1);
   const int64_t ms_per_minute = (int64_t)( 60 * 1000);
   const int64_t ms_per_hour = 60 * ms_per_minute;
   const int64_t ms_per_day = 24 * ms_per_hour;

   sprintf( text, "%02d %02d:%02d:%02d%03d",
            (int)( milliseconds / ms_per_day),
            (int)( milliseconds / ms_per_hour) % 24,
            (int)( milliseconds / ms_per_minute) % 60,
            (int)( milliseconds / 1000) % 60,
            (int)( milliseconds % 1000));
   while( precision < 3)
      text[11 + precision++] = ' ';
}

static void put_mag_resid( char *output_text, const double obs_mag,
                           const double computed_mag, const char mag_band)
{
   if( obs_mag < BLANK_MAG && computed_mag)
      sprintf( output_text, "%6.2f ",
               obs_mag - computed_mag);
//             obs_mag - computed_mag - mag_band_shift( mag_band);
   else
      strcpy( output_text, "------ ");
}

static void show_resid_in_sigmas( char *buff, const double sigmas)
{
   if( sigmas < -999 || sigmas > 999)
      strcpy( buff, " HUGE ");
   else
      sprintf( buff, (fabs( sigmas) > 9.9 ? " %+4.0f " : " %+4.1f "), sigmas);
}

/* format_observation( ) takes an observation and produces text for it,
   suitable for display on a console (findorb) or in a Windoze scroll
   box (FIND_ORB),  or for writing to a file.  */

void format_observation( const OBSERVE FAR *obs, char *text,
                                        const int resid_format)
{
   double angle;
   char xresid[30], yresid[30];
   int i;
   const int base_format = (resid_format & 3);
   const int base_time_format = obs->time_precision / 10;
   const int n_time_digits = obs->time_precision % 10;
   const int four_digit_years =
                    (resid_format & RESIDUAL_FORMAT_FOUR_DIGIT_YEARS);
   int month;
   long year;
   double day, utc;
   char *original_text_ptr = text;
   MOTION_DETAILS m;

   utc = obs->jd - td_minus_utc( obs->jd) / seconds_per_day;
   day = decimal_day_to_dmy( utc, &year, &month, CALENDAR_JULIAN_GREGORIAN);

   if( base_format != RESIDUAL_FORMAT_SHORT)
      {
      switch( base_time_format)
         {
         case 2:        /* CYYMMDD:HHMMSSsss:  formats 20-23 */
         case 3:        /* CYYMMDD.ddddddddd:  formats 30-39 */
            sprintf( text, "%c%02ld%02d",     /* show century letter, 2digit yr, mo */
                     (char)( 'A' + year / 100 - 10), year % 100,
                     month);
            if( base_time_format == 2)
               {
               show_dd_hh_mm_ss_point_sss( text + 5, day, n_time_digits);
               text[7] = ':';
               text[10] = text[11];     /* Turn dd hh:mm:ss into dd:hhmmss. This */
               text[11] = text[12];     /* corresponds to the somewhat weird way */
               text[12] = text[14];     /* in which you can specify precise times */
               text[13] = text[15];     /* within the limits of the MPC's 80-byte */
               text[14] = text[16];     /* format (a Find_Orb extension).         */
               text[15] = text[17];
               text[16] = text[18];
               text[17] = text[19];
               text[18] = '\0';
               }
            else
               sprintf( text + 5, "%012.9f", day);
            break;
         case 4:        /* M056336.641592653: MJD formats 40-49 */
         case 1:        /* 2456336.641592653: JD formats 10-49 */
            {
            sprintf( text, "%017.9f",
                           utc - (base_time_format == 4 ? 2400000.5 : 0.));
            if( base_time_format == 4)
               *text = 'M';
            }
            break;
         case 0:        /* "Standard" MPC YYYY MM DD.dddddd : formats 0-6 */
            {
            assert( n_time_digits <= 6);
            if( four_digit_years)
               sprintf( text, "%04ld\t%02d\t", year, month);
            else
               sprintf( text, "%02d\t%02d\t", abs( (int)year % 100), month);
            text += strlen( text);
            if( resid_format & RESIDUAL_FORMAT_HMS)
               show_dd_hh_mm_ss_point_sss( text, day, 0);
            else
               {
               const char *date_format_text[7] = { "%02.0f       ",
                                                   "%04.1f     ",
                                                   "%05.2f    ",
                                                   "%06.3f   ",
                                                   "%07.4f  ",
                                                   "%08.5f ",
                                                   "%09.6f" };

               sprintf( text, date_format_text[n_time_digits], day);
               }
            }
            break;
         default:
            assert( true);
            break;
         }
      if( base_time_format == 3 || base_time_format == 1 || base_time_format == 4)
         for( i = n_time_digits + 8; i < 17; i++)     /* clear excess digits */
            text[i] = ' ';
      sprintf( text + strlen( text), "\t%c\t%s\t",
                   (obs->is_included ? ' ' : 'X'), obs->mpc_code);
      angle = obs->ra * 12. / PI;
      angle = fmod( angle, 24.);
      if( angle < 0.) angle += 24.;
      output_angle_to_buff( text + strlen( text), angle, obs->ra_precision);
      strcat( text, (base_format == RESIDUAL_FORMAT_FULL_WITH_TABS) ?
                              "\t" : "\t ");
      }
   else        /* 'short' MPC format: */
      {
      if( four_digit_years)
         *text++ = int_to_mutant_hex_char( year / 100);
      sprintf( text, "%02d%02d%02d %s",
                       abs( (int)year % 100), month, (int)day, obs->mpc_code);
      }
   text += strlen( text);

   compute_observation_motion_details( obs, &m);        /* mpc_obs.cpp */
   if( obs->note2 == 'R')
      {
      RADAR_INFO rinfo;

      compute_radar_info( obs, &rinfo);
      if( !rinfo.rtt_obs)
         strcpy( xresid, " ---- ");
      else if( resid_format & RESIDUAL_FORMAT_TIME_RESIDS)
         show_resid_in_sigmas( xresid,
                   (rinfo.rtt_obs - rinfo.rtt_comp) / rinfo.rtt_sigma);
      else
         {
         const double time_resid_in_microseconds =
                   (rinfo.rtt_obs - rinfo.rtt_comp) * 1e+6;

         if( fabs( time_resid_in_microseconds) < 999.)
            {
            sprintf( xresid, "%+05d ", (int)( time_resid_in_microseconds * 10.));
            xresid[5] = xresid[0];
            xresid[0] = ' ';
            }
         else
            strcpy( xresid, " HUGE ");
         }
      if( !rinfo.doppler_obs)
         strcpy( yresid, " ---- ");
      else
         {
         const double resid_in_hz = rinfo.doppler_obs - rinfo.doppler_comp;

         if( resid_format & RESIDUAL_FORMAT_TIME_RESIDS)
            show_resid_in_sigmas( yresid, resid_in_hz / rinfo.doppler_sigma);
         else if( fabs( resid_in_hz) < 999.)
            {
            sprintf( yresid, "%+05d ", (int)( resid_in_hz * 10.));
            yresid[5] = yresid[0];
            yresid[0] = ' ';
            }
         else
            strcpy( yresid, " HUGE ");
         }
      }
   else if( resid_format & RESIDUAL_FORMAT_TIME_RESIDS)
      {
      const double abs_time_resid = fabs( m.time_residual);
      const char sign = (m.time_residual < 0. ? '-' : '+');

      if( abs_time_resid < .00094)               /* show as " -.4ms " */
         sprintf( xresid, " %c.%01dms", sign,
                     (int)( abs_time_resid * 10000. + .5));
      else if( abs_time_resid < .099)            /* show as " -47ms " */
         sprintf( xresid, " %c%02dms", sign,
                     (int)( abs_time_resid * 1000. + .5));
      else if( abs_time_resid < .994)            /* show as " +.31s " */
         sprintf( xresid, " %c.%02ds", sign,
                     (int)( abs_time_resid * 100. + .5));
      else if( abs_time_resid < 9.9)             /* show as " -4.7s " */
         sprintf( xresid, " %+4.1fs", m.time_residual);
      else if( abs_time_resid < 999.)            /* show as " -217s " */
         sprintf( xresid, " %c%03ds", sign,
                     (int)( abs_time_resid + .5));
      else if( abs_time_resid / 60. < 999.)      /* show as " +133m " */
         sprintf( xresid, " %c%03dm", sign,
                     (int)( abs_time_resid / 60. + .5));
      else if( abs_time_resid / 3600. < 9999.)   /* show as " +027h " */
         sprintf( xresid, " %c%03dh", sign,
                     (int)( abs_time_resid / 3600. + .5));
      else                                   /* Give up after 1000 hours; */
         strcpy( xresid, " !!!! ");          /* show "it's a long time"   */
      put_residual_into_text( yresid, m.cross_residual, resid_format);
      }
   else
      {
      put_residual_into_text( xresid, m.xresid, resid_format);
      put_residual_into_text( yresid, m.yresid, resid_format);
      }
   if( base_format != RESIDUAL_FORMAT_SHORT)
      {
      const char *tab_separator =
             ((base_format == RESIDUAL_FORMAT_FULL_WITH_TABS) ? "\t" : "");

      angle = obs->dec * 180. / PI;
      if( angle < 0.)
         {
         angle = -angle;
         *text++ = '-';
         if( angle < -99.)       /* error prevention */
            angle = -99.;
         }
      else
         {
         *text++ = '+';
         if( angle > 99.)       /* error prevention */
            angle = 99.;
         }
      output_angle_to_buff( text, angle, obs->dec_precision);

      sprintf( text + strlen( text), "\t%s%s%s\t", xresid, tab_separator, yresid);
      format_dist_in_buff( xresid, obs->r);
      if( resid_format & RESIDUAL_FORMAT_MAG_RESIDS)
         put_mag_resid( yresid, obs->obs_mag, obs->computed_mag, obs->mag_band);
      else
         format_dist_in_buff( yresid, obs->solar_r);
      sprintf( text + strlen( text), "%s%s%s", xresid, tab_separator, yresid);
      }
   else        /* 'short' MPC format */
      {
      if( resid_format & RESIDUAL_FORMAT_MAG_RESIDS)
         {
         put_mag_resid( yresid, obs->obs_mag, obs->computed_mag, obs->mag_band);
         put_residual_into_text( xresid, sqrt( m.xresid * m.xresid
                                             + m.yresid * m.yresid), resid_format);
         xresid[5] = ' ';        /* replace the '+' with a ' ' */
         }
      strncpy( text, xresid, 6);
      strncpy( text + 6, yresid, 6);
      text[0] = (obs->is_included ? ' ' : '(');
      text[12] = (obs->is_included ? ' ' : ')');
      text[13] = '\0';
      }
                       /* for all other formats, replace tabs w/spaces: */
   if( base_format != RESIDUAL_FORMAT_FULL_WITH_TABS)
      for( i = 0; original_text_ptr[i]; i++)
         if( original_text_ptr[i] == '\t')
            original_text_ptr[i] = ' ';
}


/* The MPC report format goes to only six decimal places in time,
a "microday".  If the reported time is more precise than that -- as can
happen with video observations -- a workaround is to make use of the object
motion data to adjust the position by up to half a microday.  We only do
this if the time given is more than a nanoday away from an integer
microday.  That simply avoids processing in the (overwhelmingly likely)
case that the data falls exactly on a microday:  i.e.,  it was given
to six or fewer decimal places.

   NOTE:  this should now be obsolete,  at least for Find_Orb,  because
several workarounds for getting up to nine decimal places -- "nanoday"
precision,  or 86.4 microseconds -- are now available.  Hence the call
to this being commented out.  I've not removed the code,  because it's
possible we'll need to reformat astrometry in a manner that'll make MPC
reasonably happy.   */

#ifdef CURRENTLY_UNUSED_POSSIBLY_OBSOLETE
static inline void set_obs_to_microday( OBSERVE FAR *obs)
{
   const double utc = obs->jd - td_minus_utc( obs->jd) / seconds_per_day;
   double delta_jd = utc - floor( utc);

   delta_jd = 1e+6 * delta_jd + .5;
   delta_jd = (delta_jd - floor( delta_jd) - .5) * 1e-6;
   if( delta_jd > 1e-9 || delta_jd < -1e-9)
      {
      MOTION_DETAILS m;
      const double cvt_motions_to_radians_per_day =
                  (PI / 180.) * 24. / 60.;

      compute_observation_motion_details( obs, &m);
      obs->jd -= delta_jd;
                  /* motions are given in '/hr, a.k.a. "/min: */
      obs->ra -= m.ra_motion * delta_jd * cvt_motions_to_radians_per_day
                        / cos( obs->dec);
      obs->dec -= m.dec_motion * delta_jd * cvt_motions_to_radians_per_day;
      }
}
#endif

void recreate_observation_line( char *obuff, const OBSERVE FAR *obs)
{
   char buff[100];
   int mag_digits_to_erase = 0;
   OBSERVE tobs = *obs;

   if( obs->note2 == 'R')     /* for radar obs,  we simply store the */
      {                       /* original observation line           */
      strcpy( obuff, obs->second_line + 81);
      return;
      }
// set_obs_to_microday( &tobs);
   format_observation( &tobs, buff, 4);
   memcpy( obuff, obs->packed_id, 12);
   obuff[12] = obs->discovery_asterisk;
   obuff[13] = obs->note1;
   obuff[14] = obs->note2;
   memcpy( obuff + 15, buff, 17);      /* date/time */
   memcpy( obuff + 32, buff + 24, 12);      /* RA */
   memcpy( obuff + 44, buff + 38, 13);      /* dec */
   sprintf( obuff + 57, "%13.2f%c%c%s%s", obs->obs_mag,
              obs->mag_band, obs->mag_band2, obs->reference, obs->mpc_code);
   if( obs->obs_mag == BLANK_MAG)        /* no mag given;  clean out that value */
      mag_digits_to_erase = 5;
   else
      mag_digits_to_erase = 2 - obs->mag_precision;
   memset( obuff + 70 - mag_digits_to_erase, ' ', mag_digits_to_erase);
   memcpy( obuff + 56, obs->columns_57_to_65, 9);
   if( !obs->is_included)
      obuff[64] = 'x';
   if( obs->flags & OBS_DONT_USE)
      obuff[64] = '!';
}

#ifdef NOT_QUITE_READY_YET
void recreate_second_observation_line( char *buff, const OBSERVE FAR *obs)
{
   int i;
   double vect[3];

   buff[32] = '0' + obs->satellite_obs;
   for( i = 0; i < 3; i++)
      vect[i] = obs->obs_posn[j] - ?; (gotta get earths loc somewhere...)
   ecliptic_to_equatorial( vect);
   for( i = 0; i < 3; i++)
      sprintf( buff + 33 + i * 12, "%12.8f", vect[i]);
   buff[69] = ' ';
}
#endif

int process_count;

/* When running on multiple cores,  we need to keep the processes running
on each core from overwriting one another's files.  Thus,  a file such as
'elements.txt' can retain that name in the single-core case.  A second
core should use 'eleme1.txt',  a third 'eleme2.txt',  and so on.  */

char *get_file_name( char *filename, const char *template_file_name)
{
   if( !process_count)
      strcpy( filename, template_file_name);
   else
      {
      const char *tptr = strchr( template_file_name, '.');
      size_t count = tptr - template_file_name;

      assert( tptr);
      assert( process_count < 1000);
      if( count > 5)
         count = 5;
      memcpy( filename, template_file_name, count);
      sprintf( filename + count, "%d%s", process_count, tptr);
      }
   return( filename);
}

void create_obs_file( const OBSERVE FAR *obs, int n_obs, const int append)
{
   char filename[81], curr_sigma_text[81];
   FILE *ofile = fopen_ext( get_file_name( filename, observe_filename),
                           append ? "fcab" : "fcwb");

   *curr_sigma_text = '\0';
   while( n_obs--)
      {
      char obuff[81];

      sprintf( obuff, "COM Posn sigma %g", obs->posn_sigma_1);
      if( obs->posn_sigma_2 != obs->posn_sigma_1)  /* elliptical sigma */
         {
         sprintf( obuff + strlen( obuff), " %g", obs->posn_sigma_2);
         if( obs->posn_sigma_theta)    /* uncertainty is a tilted ellipse */
            sprintf( obuff + strlen( obuff), " tilt %.1f",
                        obs->posn_sigma_theta * 180. / PI);
         }
      if( obs->note2 != 'R' && strcmp( curr_sigma_text, obuff))
         {
         fprintf( ofile, "%s\n", obuff);
         strcpy( curr_sigma_text, obuff);
         }
      recreate_observation_line( obuff, obs);
      fprintf( ofile, "%s\n", obuff);
      if( obs->second_line)
         fprintf( ofile, "%s\n", obs->second_line);
      obs++;
      }
   fclose( ofile);
}

static void add_final_period( char *buff)
{
   if( *buff && buff[strlen( buff) - 1] != '.')
      strcat( buff, ".");
}

static void tack_on_names( char *list, const char *names)
{
   while( *names)
      {
      int i, len, already_in_list = 0;

      while( *names == ' ')
         names++;
      for( len = 0; names[len] && names[len] != ','; len++)
         ;
      for( i = 0; list[i]; i++)
         if( !i || (i > 1 && list[i - 2] == ','))
            if( !memcmp( list + i, names, len))
               if( list[i + len] == ',' || !list[i + len])
                  already_in_list = 1;
      if( !already_in_list)
         {
         char *lptr;

         if( *list)
            strcat( list, ", ");
         lptr = list + strlen( list);
         memcpy( lptr, names, len);
         lptr[len] = '\0';
         }
      names += len;
      if( *names == ',')
         names++;
      }
}

/* It's fairly common for artificial satellite IDs to start in
somewhat arbitrary columns.  This checks to see if two packed IDs
match,  after allowing for the possibility that one is "shifted"
relative to the other.        */

static bool packed_ids_match( const char *id1, const char *id2)
{
   size_t len1 = 12, len2 = 12;

   while( *id1 == ' ' && len1)
      {
      id1++;
      len1--;
      }
   while( len1 && id1[len1 - 1] == ' ')
      len1--;

   while( *id2 == ' ' && len2)
      {
      id2++;
      len2--;
      }
   while( len2 && id2[len2 - 1] == ' ')
      len2--;
   return( len1 == len2 && !memcmp( id1, id2, len1));
}

/* In getting 'details',  there are really two scenarios.  Either we're
getting the COD/OBS/MEA/TEL data from the observation file (best case),
or we don't have those and are instead trawling for the default
observer/measurer names and telescope data from the files 'details.txt'
and 'scopes.txt' (which you should look at).

   In the first case,  'packed_id' is non-NULL.  Success consists of
reading through the file of astrometric data,  locating the correct
COD line,  getting OBS/MEA/TEL data,  and then finding a matching
'packed_id' before hitting another COD.

   In the other cases,  'packed_id' is NULL.  Again,  we read through
the file until we hit the right COD line,  again get OBS/MEA/TEL data.
But we keep on reading until we hit the next COD line.   */

static int get_observer_details( const char *observation_filename,
      const char *mpc_code, char *observers, char *measurers, char *scope,
      const char *packed_id)
{
   FILE *ifile = fopen_ext( observation_filename, "fclrb");
   int rval = 0, n_codes_found = 0;

   *observers = *measurers = *scope = '\0';
   if( ifile)
      {
      char buff[700];
      bool done = false;

      while( !done && fgets( buff, sizeof( buff), ifile))
         if( !memcmp( buff, "COD ", 4))
            {
            bool new_code_found = false;

            *observers = *measurers = *scope = '\0';
            n_codes_found++;
            if( !memcmp( buff + 4, mpc_code, 3))
               while( !done && fgets_trimmed( buff, sizeof( buff), ifile) && !new_code_found)
                  {
                  if( !memcmp( buff, "OBS ", 4))
                     tack_on_names( observers, buff + 4);
                  if( !memcmp( buff, "MEA ", 4))
                     tack_on_names( measurers, buff + 4);
                  if( !memcmp( buff, "TEL ", 4))  /* allow for only one scope */
                     strcpy( scope, buff + 4);
                  if( packed_id && packed_ids_match( packed_id, buff))
                     done = true;
                  if( !memcmp( buff, "COD ", 4))
                     {
                     if( memcmp( buff + 4, mpc_code, 3))
                        {
                        new_code_found = true;
                        if( !packed_id)
                           done = true;
                        }
                     else
                        *observers = *measurers = *scope = '\0';
                     }
                  }
            }
      fclose( ifile);
      add_final_period( observers);
      add_final_period( measurers);
      add_final_period( scope);
      if( !strcmp( observers, measurers))
         *measurers = '\0';
      }

   if( *observers)
      rval = 1;
   if( *measurers)
      rval |= 2;
   if( *scope)
      rval |= 4;
   if( !n_codes_found)        /* we can just ignore this file completely, */
      rval = -1;              /* even for other observatory codes */
   return( rval);
}

#define REPLACEMENT_COLUMN 42

static void observer_link_substitutions( char *buff)
{
   FILE *ifile = fopen_ext( "observer.txt", "fcrb");

   if( ifile)
      {
      char line[200], *loc;

      while( fgets_trimmed( line, sizeof( line), ifile))
         if( *line != ';' && *line != '#')
            {
            line[REPLACEMENT_COLUMN - 1] = '\0';
            remove_trailing_cr_lf( line);
            loc = strstr( buff, line);
            if( loc)
               {
               const size_t len = strlen( line);
               size_t len2;

               if( loc[len] <= ' ' || loc[len] == '.' || loc[len] == ',')
                  {
                  len2 = strlen( line + REPLACEMENT_COLUMN);
                  memmove( loc + len2, loc + len, strlen( loc + len) + 2);
                  memcpy( loc, line + REPLACEMENT_COLUMN, len2);
                  }
               }
            }
      fclose( ifile);
      }
}

static int write_observer_data_to_file( FILE *ofile, const char *ast_filename,
                 const int n_obs, const OBSERVE FAR *obs_data)
{
   int n_stations = 0, i, j;
   int try_ast_file = 1, try_details_file = 1, try_scope_file = 1;
   char stations[500][5];

   for( i = 0; i < n_obs; i++)
      {
      int compare = 1;

      j = 0;
      while( j < n_stations &&
             (compare = strcmp( obs_data[i].mpc_code, stations[j])) > 0)
         j++;
      if( compare)         /* got a new one */
         {
         assert( strlen( obs_data[i].mpc_code) == 3);
         memmove( stations + j + 1, stations + j, (n_stations - j)  * sizeof( stations[0]));
         strcpy( stations[j], obs_data[i].mpc_code);
         n_stations++;
         assert( n_stations < 400);
         }
      }
   for( i = 0; i < n_stations; i++)
      {
      char buff[200], tbuff[100];
      char details[3][300];
      int details_found = 0;
      size_t loc;

      FSTRCPY( tbuff, stations[i]);
      put_observer_data_in_text( tbuff, buff);
      fprintf( ofile, "(%s) %s", tbuff, buff);

      if( try_ast_file)
         {
         details_found = get_observer_details( ast_filename, tbuff,
                                 details[0], details[1], details[2],
                                 obs_data->packed_id);
         if( details_found == -1)       /* file wasn't found,  or it has */
            {                           /* no observational details.  In */
            details_found = 0;          /* either case,  ignore it for   */
            try_ast_file = 0;           /* further MPC codes.            */
            }
         }

      if( !details_found && try_details_file)
         {
         details_found = get_observer_details( "details.txt", tbuff,
                                 details[0], details[1], details[2], NULL);
         if( details_found == -1)       /* file wasn't found,  or it has */
            {                           /* no observational details.  In */
            details_found = 0;          /* either case,  ignore it for   */
            try_details_file = 0;       /* further MPC codes.            */
            }
         }

      if( !details_found && try_scope_file)
         {
         details_found = get_observer_details( "scopes.txt", tbuff,
                                 details[0], details[1], details[2], NULL);
         if( details_found == -1)       /* file wasn't found,  or it has */
            {                           /* no observational details.  In */
      /*    details_found = 0;   */     /* either case,  ignore it for   */
            try_scope_file = 0;         /* further MPC codes.            */
            }
         }

      fprintf( ofile, ".");
      loc = 7 + strlen( buff);
      for( j = 0; j < 3; j++)
         if( *details[j])
            {
            char inserted_text[15], *outtext = details[j];

            if( j == 2)
               strcpy( inserted_text, " ");
            else
               {
               strcpy( inserted_text, j ? " Measurer" : "  Observer");
               if( strchr( outtext, ','))
                  strcat( inserted_text, "s");
               strcat( inserted_text, " ");
               }
            memmove( outtext + strlen( inserted_text), outtext,
                              strlen( outtext) + 1);
            memcpy( outtext, inserted_text, strlen( inserted_text));
            while( *outtext)
               {
               bool done;
               size_t k;

               for( k = 0; outtext[k] && outtext[k] != ' '; k++)
                  ;
               done = !outtext[k];
               outtext[k] = '\0';
               if( loc + k > 78)    /* gotta go to a new line */
                  {
                  fprintf( ofile, "\n    %s", outtext);
                  loc = k + 4;
                  }
               else
                  {
                  fprintf( ofile, " %s", outtext);
                  loc += k + 1;
                  }
               outtext += k;
               if( !done)
                  outtext++;
               }
            }
      fprintf( ofile, "\n");
      }
   return( 0);
}

int write_residuals_to_file( const char *filename, const char *ast_filename,
       const int n_obs, const OBSERVE FAR *obs_data, const int resid_format)
{
   FILE *ofile = fopen_ext( filename, "fcw");
   int rval = 0;

   if( ofile )
      {
      char buff[100];
      int number_lines = (n_obs + 2) / 3;
      int i;

      if( (resid_format & 3) == RESIDUAL_FORMAT_SHORT)
         for( i = 0; i < number_lines * 3; i++)
            {
            int num = (i % 3) * number_lines + i / 3;
            OBSERVE FAR *obs = ((OBSERVE FAR *)obs_data) + num;

            if( num < n_obs)
               format_observation( obs, buff, resid_format);
            else
               *buff = '\0';
            fprintf( ofile, "%s%s", buff, (i % 3 == 2) ? "\n" : "   ");
            }
      else
         for( i = 0; i < n_obs; i++)
            {
            format_observation( obs_data + i, buff, resid_format);
            fprintf( ofile, "%s\n", buff);
            }
      fprintf( ofile, "\nStation data:\n");
      write_observer_data_to_file( ofile, ast_filename, n_obs, obs_data);
      fclose( ofile);
      }
   else                    /* file not opened */
      rval = -1;
   return( rval);
}

#ifdef FUTURE_PROJECT_IN_WORKS

int create_residual_scattergram( const char *filename, const int n_obs,
                         const OBSERVE FAR *obs)
{
   const int tbl_height = 19, tbl_width = 71;
   const int xspacing = 12, yspacing = 5,
   short *remap_table = (short *)calloc( tbl_height * tbl_width, sizeof( short));
   int i, j;
   FILE *ofile = fopen_ext( filename, "fcwb");

   for( i = 0; i < n_obs; i++, obs++)
      {
      const double yresid = 3600. * (180./pi) * (obs->dec - obs->computed_dec);
      const double xresid = 3600. * (180./pi) * (obs->ra - obs->computed_ra)
                                        * cos( obs->computed_dec);
      int xloc = (int)floor( xresid * (double)xspacing / .5)
      }

   fclose( ofile);
   free( remap_table);
}
#endif

void remove_trailing_cr_lf( char *buff)
{
   int i;

   for( i = 0; buff[i] && buff[i] != 13 && buff[i] != 10; i++)
      ;
   while( i && buff[i - 1] == ' ')
      i--;
   buff[i] = '\0';
}

/* MPC frowns upon redistribution of NEOCP astrometry.  So if an input
line is from NEOCP,  it's blacked out,  _unless_ it's from a station
that has given permission for republication.  Those stations are listed
in the GREENLIT line in 'environ.dat';  you can add your own if desired.

For private use,  you can turn NEOCP redaction off... just be sure that
if you do that,  you don't redistribute anything.        */

bool neocp_redaction_turned_on = true;

static bool line_must_be_redacted( const char *mpc_line)
{
   return( strlen( mpc_line) == 80 && neocp_redaction_turned_on
               &&  !memcmp( mpc_line + 72, "NEOCP", 5)
               && !strstr( get_environment_ptr( "GREENLIT"), mpc_line + 77));
}

int text_search_and_replace( char FAR *str, const char *oldstr,
                                     const char *newstr)
{
   size_t ilen = FSTRLEN( str), rval = 0;
   const size_t oldlen = strlen( oldstr);
   const size_t newlen = strlen( newstr);

   while( ilen >= oldlen)
      if( !FMEMCMP( str, oldstr, oldlen))
         {
         FMEMMOVE( str + newlen, str + oldlen, ilen - oldlen + 1);
         FMEMCPY( str, newstr, newlen);
         str += newlen;
         ilen -= oldlen;
         rval++;
         }
      else
         {
         str++;
         ilen--;
         }
   return( (int)rval);
}

static long round_off( const double ival, const double prec)
{
   long rval = 0L, digit;
   int got_it = 0;
   double diff;

   for( digit = 10000000L; !got_it; digit /= 10)
      {
      rval = (((long)ival + digit / 2L) / digit) * digit;
      diff = fabs( (double)rval - ival);
      if( digit == 1 || diff < ival * prec)
         got_it = 1;
      }
   return( rval);
}

char *mpec_error_message = NULL;

int make_pseudo_mpec( const char *mpec_filename, const char *obj_name)
{
   char buff[500], mpec_buff[7];
   FILE *ofile = fopen_ext( mpec_filename, "fcwb");
   FILE *header_htm_ifile;
   FILE *residuals_ifile, *ephemeris_ifile, *observations_ifile;
   FILE *elements_file = fopen_ext( get_file_name( buff, elements_filename), "fcrb");
   int line_no = 0, rval = 0, total_lines = 0;
   int mpec_no = atoi( get_environment_ptr( "MPEC"));
   bool orbit_is_heliocentric = true, suppressed = false;
   extern char findorb_language;

   assert( ofile);
   if( elements_file)
      {
      unsigned i;

      for( i = 0; i < 10 && fgets( buff, sizeof( buff), elements_file); i++)
         if( i == 1 && !memcmp( buff, "   Perihel", 10))
            i = 10;     /* yes,  it's definitely heliocentric,  no need to go on */
         else if( *buff == 'P')
            orbit_is_heliocentric = false;
      }

   if( mpec_no)
      sprintf( mpec_buff, "_%02x", mpec_no % 256);
   else
      *mpec_buff = '\0';
   sprintf( buff, "%cheader.htm", findorb_language);
   header_htm_ifile = fopen_ext( buff, "crb");
   if( !header_htm_ifile)
      header_htm_ifile = fopen_ext( buff + 1, "fcrb");
   if( header_htm_ifile)                 /* copy header data to pseudo-MPEC */
      {
      while( fgets( buff, sizeof( buff), header_htm_ifile) &&
                           memcmp( buff, "(End of header)", 15))
         if( *buff != '#')
            {
            char *tptr = strstr( buff, "_xx");

            if( tptr)
               {
               memmove( tptr + strlen( mpec_buff), tptr + 3, strlen( tptr));
               memcpy( tptr, mpec_buff, strlen( mpec_buff));
               }
            if( !memcmp( buff, "$Error", 6))
               {
               if( !mpec_error_message)
                  *buff = '\0';
               else
                  {
                  strcpy( buff, "<p> <b>");
                  strcat( buff, mpec_error_message);
                  strcat( buff, "</b> </p>");
                  }
               }
            while( (tptr = strchr( buff, '$')) != NULL)
               {                       /* See comments in 'header.htm'.  */
               int got_it = 0, i;      /* code replaces text between $s  */
                                       /* in that file.                  */
               for( i = 1; tptr[i] && tptr[i] != '$'; i++)
                  ;        /* search for matching $ */
               if( i < 20 && tptr[i] == '$')
                  {
                  char search_str[80], replace_str[80];

                  memcpy( search_str, tptr, i);
                  search_str[i] = '\0';
                  if( elements_file)
                     {
                     char tbuff[300], *tptr2;

                     if( !strcmp( search_str, "$Tg"))
                        {
                        full_ctime( replace_str, current_jd( ),
                                       FULL_CTIME_YMD);
                        got_it = 1;
                        }
                     else if( !strcmp( search_str, "$Name"))
                        {
                        strcpy( replace_str, obj_name);
                        got_it = 1;
                        }

                     fseek( elements_file, 0L, SEEK_SET);
                     while( !got_it &&
                             fgets_trimmed( tbuff, sizeof( tbuff), elements_file))
                        if( (tptr2 = strstr( tbuff, search_str)) != NULL
                                    && tptr2[i] == '=')
                           {
                           tptr2 += i + 1;
                           for( i = 0; tptr2[i] > ' '; i++)
                              replace_str[i] = tptr2[i];
                           replace_str[i] = '\0';
                           got_it = 1;
                           }
                     strcat( search_str, "$");
                     if( got_it)
                        text_search_and_replace( buff, search_str, replace_str);
                     }
                  }
               else        /* no matching '$' found */
                  *tptr = '!';
               }
            if( !suppressed)
               fputs( buff, ofile);
            }
         else if( !memcmp( buff, "# helio_only", 12) && !orbit_is_heliocentric)
            suppressed = (buff[13] == '1');
      fclose( header_htm_ifile);
      }

   if( mpec_no)
      {
      sprintf( buff, "%d", mpec_no % 255 + 1);
      set_environment_ptr( "MPEC", buff);
      }

   observations_ifile = fopen_ext( get_file_name( buff, observe_filename), "fcrb");
   assert( observations_ifile);
   if( observations_ifile)
      {
      unsigned redacted_line_number = 0, n_redacted_lines = 0;

                  /* First,  count number of redacted lines... */
      while( fgets_trimmed( buff, sizeof( buff), observations_ifile))
         if( line_must_be_redacted( buff))
            n_redacted_lines++;
      fseek( observations_ifile, 0L, SEEK_SET);
      while( fgets_trimmed( buff, sizeof( buff), observations_ifile))
         if( *buff != '#')       /* may be comments or 'sigma' lines */
            {
//          if( buff[14] == 's' || buff[14] == 'v' || buff[14] == 'r')
//             fprintf( ofile, "%s\n", buff);
//          else
               {
               char mpc_code[8];
               const bool redacted = line_must_be_redacted( buff);

               strcpy( mpc_code, buff + 77);
               buff[77] = '\0';
               if( buff[14] != 's' && buff[14] != 'v' && buff[14] != 'r')
                  total_lines++;
               fprintf( ofile, "<a name=\"o%s%03d\"></a><a href=\"#r%s%03d\">%.12s</a>",
                        mpec_buff, total_lines, mpec_buff, total_lines, buff);
               if( redacted)
                  {
                  int i;
                  const size_t start_of_redacted_text = 25;
                  const size_t length_of_redacted_text = 77 - start_of_redacted_text;
                  char *tptr = buff + start_of_redacted_text;

                  strcpy( tptr, "<code class=\"neocp\">");
                  tptr += strlen( tptr);
                  memset( tptr, '~', length_of_redacted_text);
                  strcpy( tptr + length_of_redacted_text, "</code>");
                  for( i = 3; i >= 0; i--)
                     if( redacted_line_number == (i * (n_redacted_lines - 1) + 1) / 3)
                        {
                        char tbuff[80];
                        const char *terms[4] = { "Astrometry", "redacted;",
                                    "see", "NEOCP" };

                        strcpy( tbuff, "</code>");
                        strcat( tbuff, terms[i]);
                        strcat( tbuff, "<code class=\"neocp\">");
                        memcpy( tptr + i * 15 + 2, terms[i], strlen( terms[i]));
                        text_search_and_replace( tptr, terms[i], tbuff);
                        if( i == 3)
                           text_search_and_replace( tptr, terms[i],
                                 "<a href=\"http://www.minorplanetcenter.net/iau/NEO/ToConfirm.html\">NEOCP</a>");
                        }
                  for( i = 0; tptr[i]; i++)  /* replace all the tildes with */
                     if( tptr[i] == '~')     /* pseudorandom text, but skip */
                        {                    /* chars that can't be used in */
                        const char *forbidden = "~ <>\"&";          /* HTML */

                        while( strchr( forbidden, tptr[i]))
                           tptr[i] = (char)( ' ' + rand( ) % ('z' - ' '));
                        }
                  redacted_line_number++;
                  }
               else
                  {
                  text_search_and_replace( buff + 13, "&", "&amp;");
                  text_search_and_replace( buff + 13, "<", "&lt;");
                  text_search_and_replace( buff + 13, ">", "&gt;");
                  }
               text_search_and_replace( buff + 13, "JPLRS",
                     "<a href=\"http://ssd.jpl.nasa.gov/?radar\">JPLRS</a>");
               fprintf( ofile, " %s<a href=\"#stn_%s\">%s</a>\n",
                        buff + 13, mpc_code, mpc_code);
               }
            }
      fclose( observations_ifile);
      }
   else
      rval |= 1;

   residuals_ifile = fopen_ext( get_file_name( buff, residual_filename), "fcrb");
   if( residuals_ifile)
      {
      FILE *obslinks_file = fopen_ext( "obslinks.htm", "fcrb");
      FILE *mpc_obslinks_file = fopen_ext( "ObsCodesF.html", "fcrb");
      long obslinks_header_len = 0, mpc_obslinks_header_len = 0;
      char url[200];
              /* In making a pseudo-MPEC,  we attempt to include links     */
              /* to the Web sites of the observatory codes mentioned.  The */
              /* files 'obslinks.htm' (my effort at a complete list of Web */
              /* sites of observatory codes) and 'ObsLinksF.html' (the     */
              /* MPC's list) are both searched,  in that order.            */

      while( fgets( url, sizeof( url), obslinks_file)
                     && memcmp( url, "<a name=\"0\">", 12))
         ;
      obslinks_header_len = ftell( obslinks_file);
      while( fgets( url, sizeof( url), mpc_obslinks_file)
                     && memcmp( url, "<pre>", 5))
         ;
      mpc_obslinks_header_len = ftell( mpc_obslinks_file);
      while( fgets( buff, sizeof( buff), residuals_ifile) && memcmp( buff, "Station", 7))
         ;
      fprintf( ofile, "<a name=\"stations\"></a>\n");
      fprintf( ofile, "<b>%s</b>", buff);
      while( fgets_trimmed( buff, sizeof( buff), residuals_ifile))
         if( *buff == ' ')
            {
            observer_link_substitutions( buff);
            fprintf( ofile, "%s\n", buff);
            }
         else
            {
            char tbuff[4];
            int compare = 1, i, url_index = 0;
            char *latlon = NULL, *remains;

            memcpy( tbuff, buff + 1, 3);
            tbuff[3] = '\0';
            fprintf( ofile, "<a name=\"stn_%s\"></a>", tbuff);

            for( i = 5; !latlon && buff[i]; i++)
               if( !memcmp( buff + i - 2, "  (", 3))
                  if( (buff[i + 1] == 'N' || buff[i + 1] == 'S'))
                     {        /* found opening paren of lat/lon */
                     latlon = buff + i + 1;
                     i = 0;
                     while( latlon[i] && latlon[i] != ')')
                        i++;
                     if( latlon[i] == ')')    /* found closing paren */
                        {
                        remains = latlon + i;
                        *remains = latlon[-3] = '\0';
                        }
                     else          /* oops!  didn't find lat/lon after all */
                        latlon = NULL;
                     }

            if( !latlon)  /*  no lat/lon;  assume name ends with a . */
               {
               for( i = 0; buff[i] && buff[i] != '.'; i++)
                  ;
               if( buff[i] == '.' && buff[i + 1])        /* found the '.'! */
                  {
                  buff[i + 1] = '\0';
                  remains = buff + i + 1;
                  }
               else
                  remains = buff + i;
               }
            if( compare)
               {
               char text_to_find[50], *tptr;

               sprintf( text_to_find, "></a> %.3s  <", buff + 1);
               url[19] = '\0';
               fseek( obslinks_file, obslinks_header_len, SEEK_SET);
               while( (compare = memcmp( url + 13, text_to_find, 12)) != 0 &&
                                 fgets_trimmed( url, sizeof( url), obslinks_file))
                  ;
               tptr = strstr( url, "<br>");
               if( tptr)
                  *tptr = '\0';
               url_index = 23;   /* if there is a link,  it starts in byte 23 */
               }
            if( compare)   /* still don't have URL;  try ObsLinks.html */
               {
               *url = '\0';
               fseek( mpc_obslinks_file, mpc_obslinks_header_len, SEEK_SET);
               while( (compare = memcmp( url, buff + 1, 3)) != 0 &&
                                 fgets_trimmed( url, sizeof( url), mpc_obslinks_file))
                  ;
               url_index = 32;   /* if there is a link,  it starts in byte 32 */
               }

            if( !compare)   /* we got a link to an observatory code */
               {
               buff[5] = '\0';
               fprintf( ofile, "%s%s", buff, url + url_index);
               }
            else
               fprintf( ofile, "%s", buff);

            if( latlon)
               {
               double lat, lon;
               char lat_sign, lon_sign;

               sscanf( latlon, "%c%lf %c%lf", &lat_sign, &lat, &lon_sign, &lon);
               if( lat_sign == 'S')
                  lat = -lat;
               if( lon_sign == 'W')
                  lon = -lon;
               fprintf( ofile, " (<a title=\"Click for map\"");
               fprintf( ofile, " href=\"http://maps.google.com/maps?q=%.5f,+%.5f\">",
                              lat, lon);
               fprintf( ofile, "%s</a>)", latlon);
               }
            observer_link_substitutions( remains + 1);
            fprintf( ofile, "%s\n", remains + 1);
            }
      fclose( obslinks_file);
      fclose( mpc_obslinks_file);
      }
   else
      rval |= 2;

   if( elements_file)
      {
      bool in_comments = false;

      fseek( elements_file, 0L, SEEK_SET);
      fprintf( ofile, "<a name=\"elements%s\"></a>\n", mpec_buff);
      line_no = 0;
      while( fgets_trimmed( buff, sizeof( buff), elements_file))
         if( *buff != '#')
            {
            char *h_ptr = NULL;

            if( buff[19] == 'H')
               h_ptr = buff + 20;
            if( buff[27] == 'H')
               h_ptr = buff + 28;
            if( !line_no)
               fprintf( ofile, "<b>%s</b>\n", buff);
            else if( *buff == 'P' && h_ptr)
               {
               const double abs_mag = atof( h_ptr);
                        /* H=4 indicates 420 to 940 km,  so: */
               double upper_size = 940. * exp( (4. - abs_mag) * LOG_10 / 5.);
               const char *units = "km";
               const char *size_url =
                   "href=\"http://www.minorplanetcenter.net/iau/lists/Sizes.html\">";
               char title[50];

               h_ptr[-1] = '\0';
               if( upper_size < .004)   /* under four meters,  use cm as units: */
                  {
                  upper_size *= 1000. * 100.;
                  units = "cm";
                  }
               else if( upper_size < 4.)  /* under four km,  use meters: */
                  {
                  upper_size *= 1000.;
                  units = "meters";
                  }
               sprintf( title, "\"Size is probably %ld to %ld %s\"\n",
                          round_off( upper_size / sqrt( 5.), .1),
                          round_off( upper_size, .1), units);
               fprintf( ofile, "%s<a title=%s%sH</a>%s\n",
                           buff, title, size_url, h_ptr);
               }
            else
               {
               text_search_and_replace( buff, "<HUGE>", "&lt;HUGE&gt;");
               text_search_and_replace( buff, "m^2", "m<sup>2</sup>");
               text_search_and_replace( buff, "   Find_Orb",
                                "   <a href=\"https://www.projectpluto.com/find_orb.htm\">Find_Orb</a>");
               fprintf( ofile, "%s\n", buff);
               }
            line_no++;
            }
         else          /* put 'elements.txt' comments in the pseudo-mpec, */
            {                          /* still as comments */
            if( !in_comments)
               fprintf( ofile, "%s", "</pre> ");
            fprintf( ofile, "<!-- %s -->\n", buff + 2);
            in_comments = true;
            }
      fclose( elements_file);
      }
   else
      rval |= 4;

               /* _now_ write out residuals: */
   if( residuals_ifile)
      {
      fseek( residuals_ifile, 0L, SEEK_SET);
      fprintf( ofile, "<pre><b><a name=\"residuals%s\">Residuals in arcseconds:</a> </b>\n",
                                                         mpec_buff);
      line_no = 0;
      while( fgets( buff, sizeof( buff), residuals_ifile) && *buff > ' ')
         {
         int i, column_off = (total_lines + 2) / 3, line;

         line_no++;
         for( i = 0; i < 3; i++)
            if( (line = line_no + column_off * i) <= total_lines)
               {
               char *tptr = buff + i * 26, tbuff[20];

               tptr[6] = '\0';         /* put out the YYMMDD... */
               fprintf( ofile, "<a name=\"r%s%03d\"></a><a href=\"#o%s%03d\">%s</a>",
                        mpec_buff, line, mpec_buff, line, tptr);

               memcpy( tbuff, tptr + 7, 3);     /* ...then the obs code.. */
               tbuff[3] = '\0';
               fprintf( ofile, " <a href=\"#stn_%s\">%s</a>", tbuff, tbuff);

               tptr[23] = '\0';        /* ...and finally,  the residuals */
               strcpy( tbuff, tptr + 10);
               text_search_and_replace( tbuff, "u", "&#xb5;");
               fprintf( ofile, "%s   ", tbuff);
               }
         fprintf( ofile, "\n");
         }
      fclose( residuals_ifile);
      }

               /* ...and now,  the ephemeris: */
   ephemeris_ifile = fopen_ext( get_file_name( buff, ephemeris_filename), "fcr");
   if( ephemeris_ifile && fgets_trimmed( buff, sizeof( buff), ephemeris_ifile))
      {
      fprintf( ofile, "\n<a name=\"eph%s\"></a>", mpec_buff);
      if( *buff != '#')        /* non-observables ephemeris,  no MPC code */
         fprintf( ofile, "<b>Ephemerides:</b>\n");
      else if( !memcmp( buff + 2, "500", 3))
         fprintf( ofile, "<b>Ephemerides (geocentric):</b>\n");
      else
         fprintf( ofile, "<b>Ephemerides for %s:</b>\n", buff + 1);

      while( fgets( buff, sizeof( buff), ephemeris_ifile))
         fputs( buff, ofile);
      fclose( ephemeris_ifile);
      }
   else
      rval |= 8;

   fprintf( ofile, "</pre></body></html>\n");
   fclose( ofile);
   return( rval);
}

