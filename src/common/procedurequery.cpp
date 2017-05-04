/*****************************************************************************
* Copyright 2015-2017 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "common/procedurequery.h"
#include "navapp.h"
#include "common/proctypes.h"
#include "mapgui/mapquery.h"
#include "geo/calculations.h"
#include "common/unit.h"
#include "common/constants.h"
#include "geo/line.h"

#include "sql/sqlquery.h"

using atools::sql::SqlQuery;
using atools::geo::Pos;
using atools::geo::Rect;
using atools::geo::Line;
using atools::geo::LineString;
using atools::contains;
using atools::geo::meterToNm;
using atools::geo::opposedCourseDeg;
using atools::geo::nmToMeter;
using atools::geo::normalizeCourse;
using proc::MapProcedureLegs;
using proc::MapProcedureLeg;
using proc::MapAltRestriction;

ProcedureQuery::ProcedureQuery(atools::sql::SqlDatabase *sqlDb, MapQuery *mapQueryParam)
  : db(sqlDb), mapQuery(mapQueryParam)
{
}

ProcedureQuery::~ProcedureQuery()
{
  deInitQueries();
}

const proc::MapProcedureLegs *ProcedureQuery::getApproachLegs(const map::MapAirport& airport, int approachId)
{
  return fetchApproachLegs(airport, approachId);
}

int ProcedureQuery::approachIdForTransitionId(int transitionId)
{
  int approachId = -1;
  approachIdForTransQuery->bindValue(":id", transitionId);
  approachIdForTransQuery->exec();
  if(approachIdForTransQuery->next())
    approachId = approachIdForTransQuery->value("approach_id").toInt();
  approachIdForTransQuery->finish();
  return approachId;
}

const proc::MapProcedureLegs *ProcedureQuery::getTransitionLegs(const map::MapAirport& airport,
                                                                int transitionId)
{
  return fetchTransitionLegs(airport, approachIdForTransitionId(transitionId), transitionId);
}

const proc::MapProcedureLeg *ProcedureQuery::getApproachLeg(const map::MapAirport& airport, int approachId,
                                                            int legId)
{
#ifndef DEBUG_APPROACH_NO_CACHE
  if(approachLegIndex.contains(legId))
  {
    // Already in index
    std::pair<int, int> val = approachLegIndex.value(legId);

    // Ensure it is in the cache - reload if needed
    const MapProcedureLegs *legs = getApproachLegs(airport, val.first);
    if(legs != nullptr)
      return &legs->at(approachLegIndex.value(legId).second);
  }
  else
#endif
  {
    // Ensure it is in the cache - reload if needed
    const MapProcedureLegs *legs = getApproachLegs(airport, approachId);
    if(legs != nullptr && approachLegIndex.contains(legId))
      // Use index to get leg
      return &legs->at(approachLegIndex.value(legId).second);
  }
  qWarning() << "approach leg with id" << legId << "not found";
  return nullptr;
}

const proc::MapProcedureLeg *ProcedureQuery::getTransitionLeg(const map::MapAirport& airport, int legId)
{
#ifndef DEBUG_APPROACH_NO_CACHE
  if(transitionLegIndex.contains(legId))
  {
    // Already in index
    std::pair<int, int> val = transitionLegIndex.value(legId);

    // Ensure it is in the cache - reload if needed
    const MapProcedureLegs *legs = getTransitionLegs(airport, val.first);

    if(legs != nullptr)
      return &legs->at(transitionLegIndex.value(legId).second);
  }
  else
#endif
  {
    // Get transition ID for leg
    transitionIdForLegQuery->bindValue(":id", legId);
    transitionIdForLegQuery->exec();
    if(transitionIdForLegQuery->next())
    {
      const MapProcedureLegs *legs = getTransitionLegs(airport, transitionIdForLegQuery->value("id").toInt());
      if(legs != nullptr && transitionLegIndex.contains(legId))
        return &legs->at(transitionLegIndex.value(legId).second);
    }
    transitionIdForLegQuery->finish();
  }
  qWarning() << "transition leg with id" << legId << "not found";
  return nullptr;
}

proc::MapProcedureLeg ProcedureQuery::buildApproachLegEntry(const map::MapAirport& airport)
{
  MapProcedureLeg leg;
  leg.legId = approachLegQuery->value("approach_leg_id").toInt();
  leg.missed = approachLegQuery->value("is_missed").toBool();
  buildLegEntry(approachLegQuery, leg, airport);
  return leg;
}

proc::MapProcedureLeg ProcedureQuery::buildTransitionLegEntry(const map::MapAirport& airport)
{
  MapProcedureLeg leg;

  leg.legId = transitionLegQuery->value("transition_leg_id").toInt();

  // entry.dmeNavId = transitionLegQuery->value("dme_nav_id").toInt();
  // entry.dmeRadial = transitionLegQuery->value("dme_radial").toFloat();
  // entry.dmeDistance = transitionLegQuery->value("dme_distance").toFloat();
  // entry.dmeIdent = transitionLegQuery->value("dme_ident").toString();
  // if(!transitionLegQuery->isNull("dme_nav_id"))
  // {
  // entry.dme = mapQuery->getVorById(entry.dmeNavId);
  // entry.dmePos = entry.dme.position;
  // }

  leg.missed = false;
  buildLegEntry(transitionLegQuery, leg, airport);
  return leg;
}

void ProcedureQuery::buildLegEntry(atools::sql::SqlQuery *query, proc::MapProcedureLeg& leg,
                                   const map::MapAirport& airport)
{
  leg.type = proc::procedureLegEnum(query->value("type").toString());

  leg.turnDirection = query->value("turn_direction").toString();

  leg.fixType = query->value("fix_type").toString();
  leg.fixIdent = query->value("fix_ident").toString();
  leg.fixRegion = query->value("fix_region").toString();
  // query->value("fix_airport_ident");
  leg.recFixType = query->value("recommended_fix_type").toString();
  leg.recFixIdent = query->value("recommended_fix_ident").toString();
  leg.recFixRegion = query->value("recommended_fix_region").toString();
  leg.flyover = query->value("is_flyover").toBool();
  leg.trueCourse = query->value("is_true_course").toBool();
  leg.course = query->value("course").toFloat();
  leg.distance = query->value("distance").toFloat();
  leg.time = query->value("time").toFloat();
  leg.theta = query->value("theta").toFloat();
  leg.rho = query->value("rho").toFloat();

  leg.calculatedDistance = 0.f;
  leg.calculatedTrueCourse = 0.f;
  leg.disabled = false;
  leg.intercept = false;

  float alt1 = query->value("altitude1").toFloat();
  float alt2 = query->value("altitude2").toFloat();

  if(!query->isNull("alt_descriptor") && (alt1 > 0.f || alt2 > 0.f))
  {
    QString descriptor = query->value("alt_descriptor").toString();

    if(descriptor == "A")
      leg.altRestriction.descriptor = MapAltRestriction::AT;
    else if(descriptor == "+")
      leg.altRestriction.descriptor = MapAltRestriction::AT_OR_ABOVE;
    else if(descriptor == "-")
      leg.altRestriction.descriptor = MapAltRestriction::AT_OR_BELOW;
    else if(descriptor == "B")
      leg.altRestriction.descriptor = MapAltRestriction::BETWEEN;
    else
      leg.altRestriction.descriptor = MapAltRestriction::AT;

    leg.altRestriction.alt1 = alt1;
    leg.altRestriction.alt2 = alt2;
  }
  else
  {
    leg.altRestriction.descriptor = MapAltRestriction::NONE;
    leg.altRestriction.alt1 = 0.f;
    leg.altRestriction.alt2 = 0.f;
  }

  leg.magvar = map::INVALID_MAGVAR;

  // Load full navaid information for fix and set fix position
  if(leg.fixType == "W" || leg.fixType == "TW")
  {
    mapObjectByIdent(leg.navaids, map::WAYPOINT, leg.fixIdent, leg.fixRegion,
                     QString(), airport.position);
    if(!leg.navaids.waypoints.isEmpty())
    {
      leg.fixPos = leg.navaids.waypoints.first().position;
      leg.magvar = leg.navaids.waypoints.first().magvar;
      leg.navId = leg.navaids.waypoints.first().id;
    }
  }
  else if(leg.fixType == "V")
  {
    mapObjectByIdent(leg.navaids, map::VOR, leg.fixIdent, leg.fixRegion, QString(), airport.position);
    if(!leg.navaids.vors.isEmpty())
    {
      leg.fixPos = leg.navaids.vors.first().position;
      leg.magvar = leg.navaids.vors.first().magvar;
      leg.navId = leg.navaids.vors.first().id;
    }
  }
  else if(leg.fixType == "N" || leg.fixType == "TN")
  {
    mapObjectByIdent(leg.navaids, map::NDB, leg.fixIdent, leg.fixRegion, QString(), airport.position);
    if(!leg.navaids.ndbs.isEmpty())
    {
      leg.fixPos = leg.navaids.ndbs.first().position;
      leg.magvar = leg.navaids.ndbs.first().magvar;
      leg.navId = leg.navaids.ndbs.first().id;
    }
  }
  else if(leg.fixType == "L")
  {
    mapObjectByIdent(leg.navaids, map::ILS, leg.fixIdent, QString(), airport.ident, airport.position);
    if(!leg.navaids.ils.isEmpty())
    {
      leg.fixPos = leg.navaids.ils.first().position;
      leg.magvar = leg.navaids.ils.first().magvar;
      leg.navId = leg.navaids.ils.first().id;
    }
    else
    {
      // Use a VOR or DME as fallback
      mapObjectByIdent(leg.navaids, map::VOR, leg.fixIdent, QString(), airport.ident, airport.position);
      if(!leg.navaids.vors.isEmpty())
      {
        leg.fixPos = leg.navaids.vors.first().position;
        leg.magvar = leg.navaids.vors.first().magvar;
        leg.navId = leg.navaids.vors.first().id;
      }
    }
  }
  else if(leg.fixType == "R")
  {
    runwayEndByName(leg.navaids, leg.fixIdent, airport);
    leg.fixPos = leg.navaids.runwayEnds.isEmpty() ? airport.position : leg.navaids.runwayEnds.first().position;
    leg.navId = -1;
  }

  // Load navaid information for recommended fix and set fix position
  // Also update magvar if not already set
  map::MapSearchResult recResult;
  if(leg.recFixType == "W" || leg.recFixType == "TW")
  {
    mapObjectByIdent(recResult, map::WAYPOINT, leg.recFixIdent, leg.recFixRegion,
                     QString(), airport.position);
    if(!recResult.waypoints.isEmpty())
    {
      leg.recFixPos = recResult.waypoints.first().position;
      leg.recNavId = recResult.waypoints.first().id;

      if(!(leg.magvar < map::INVALID_MAGVAR))
        leg.magvar = recResult.waypoints.first().magvar;
    }
  }
  else if(leg.recFixType == "V")
  {
    mapObjectByIdent(recResult, map::VOR, leg.recFixIdent, leg.recFixRegion, QString(), airport.position);
    if(!recResult.vors.isEmpty())
    {
      leg.recFixPos = recResult.vors.first().position;
      leg.recNavId = recResult.vors.first().id;

      if(!(leg.magvar < map::INVALID_MAGVAR))
        leg.magvar = recResult.vors.first().magvar;
    }
  }
  else if(leg.recFixType == "N" || leg.recFixType == "TN")
  {
    mapObjectByIdent(recResult, map::NDB, leg.recFixIdent, leg.recFixRegion, QString(), airport.position);
    if(!recResult.ndbs.isEmpty())
    {
      leg.recFixPos = recResult.ndbs.first().position;
      leg.recNavId = recResult.ndbs.first().id;

      if(!(leg.magvar < map::INVALID_MAGVAR))
        leg.magvar = recResult.ndbs.first().magvar;
    }
  }
  else if(leg.recFixType == "L")
  {
    mapObjectByIdent(recResult, map::ILS, leg.recFixIdent, QString(), airport.ident, airport.position);
    if(!recResult.ils.isEmpty())
    {
      leg.recFixPos = recResult.ils.first().position;
      leg.recNavId = recResult.ils.first().id;

      if(!(leg.magvar < map::INVALID_MAGVAR))
        leg.magvar = recResult.ils.first().magvar;
    }
    else
    {
      // Use a VOR or DME as fallback
      mapObjectByIdent(recResult, map::VOR, leg.recFixIdent, QString(), airport.ident, airport.position);
      if(!recResult.vors.isEmpty())
      {
        leg.recFixPos = recResult.vors.first().position;
        leg.recNavId = recResult.vors.first().id;

        if(!(leg.magvar < map::INVALID_MAGVAR))
          leg.magvar = recResult.vors.first().magvar;
      }
    }
  }
  else if(leg.recFixType == "R")
  {
    runwayEndByName(recResult, leg.recFixIdent, airport);
    leg.recFixPos = recResult.runwayEnds.isEmpty() ? airport.position : recResult.runwayEnds.first().position;
    leg.recNavId = -1;
  }
}

/* Get runway end and try lower and higher numbers if nothing was found - adds a dummy entry with airport
 * position if no runway ends were found */
void ProcedureQuery::runwayEndByName(map::MapSearchResult& result, const QString& name, const map::MapAirport& airport)
{
  QString bestRunway = map::runwayBestFit(name, mapQuery->getRunwayNames(airport.id));

  if(!bestRunway.isEmpty())
    mapQuery->getMapObjectByIdent(result, map::RUNWAYEND, bestRunway, QString(), airport.ident);

  if(result.runwayEnds.isEmpty())
  {
    // Get heading of runway by name
    int rwnum = 0;
    map::runwayNameSplit(name, &rwnum);

    // Create a dummy with the airport position as the last resort
    map::MapRunwayEnd end;
    end.name = name;
    end.heading = rwnum * 10.f;
    end.position = airport.position;
    end.secondary = false;
    result.runwayEnds.append(end);

    qWarning() << "Created runway dummy" << name << "for airport" << airport.ident;
  }
  else if(result.runwayEnds.first().name != name)
    qWarning() << "Found runway" << result.runwayEnds.first().name
               << "as replacement for" << name << "airport" << airport.ident;

}

void ProcedureQuery::mapObjectByIdent(map::MapSearchResult& result, map::MapObjectTypes type,
                                      const QString& ident, const QString& region, const QString& airport,
                                      const Pos& sortByDistancePos)
{
  mapQuery->getMapObjectByIdent(result, type, ident, region, airport, sortByDistancePos);
  if(result.isEmpty(type))
    mapQuery->getMapObjectByIdent(result, type, ident, QString(), airport, sortByDistancePos,
                                  atools::geo::nmToMeter(200.f));
}

void ProcedureQuery::updateMagvar(const map::MapAirport& airport, proc::MapProcedureLegs& legs)
{
  // Calculate average magvar for all legs
  float avgMagvar = 0.f;
  float num = 0.f;
  for(int i = 0; i < legs.size(); i++)
  {
    if(legs.at(i).magvar < map::INVALID_MAGVAR)
    {
      avgMagvar += legs.at(i).magvar;
      num++;
    }
  }

  if(num > 0)
    avgMagvar /= num;
  else
    // Use magnetic variance of the airport if nothing found
    avgMagvar = airport.magvar;

  // Assign average to legs with no magvar
  for(MapProcedureLeg& leg : legs.approachLegs)
  {
    if(!(leg.magvar < map::INVALID_MAGVAR))
      leg.magvar = avgMagvar;
  }

  for(MapProcedureLeg& leg : legs.transitionLegs)
  {
    if(!(leg.magvar < map::INVALID_MAGVAR))
      leg.magvar = avgMagvar;
  }
}

void ProcedureQuery::updateBounding(proc::MapProcedureLegs& legs)
{
  for(int i = 0; i < legs.size(); i++)
  {
    const proc::MapProcedureLeg& leg = legs.at(i);
    legs.bounding.extend(leg.fixPos);
    legs.bounding.extend(leg.interceptPos);
    legs.bounding.extend(leg.line.boundingRect());
  }
}

proc::MapProcedureLegs *ProcedureQuery::fetchApproachLegs(const map::MapAirport& airport, int approachId)
{
#ifndef DEBUG_APPROACH_NO_CACHE
  if(approachCache.contains(approachId))
    return approachCache.object(approachId);
  else
#endif
  {
    qDebug() << "buildApproachEntries" << airport.ident << "approachId" << approachId;

    MapProcedureLegs *legs = buildApproachLegs(airport, approachId);
    postProcessLegs(airport, *legs);

    for(int i = 0; i < legs->size(); i++)
      approachLegIndex.insert(legs->at(i).legId, std::make_pair(approachId, i));

    approachCache.insert(approachId, legs);
    return legs;
  }
}

proc::MapProcedureLegs *ProcedureQuery::fetchTransitionLegs(const map::MapAirport& airport,
                                                            int approachId, int transitionId)
{
#ifndef DEBUG_APPROACH_NO_CACHE
  if(transitionCache.contains(transitionId))
    return transitionCache.object(transitionId);
  else
#endif
  {
    qDebug() << "buildApproachEntries" << airport.ident << "approachId" << approachId
             << "transitionId" << transitionId;

    transitionLegQuery->bindValue(":id", transitionId);
    transitionLegQuery->exec();

    proc::MapProcedureLegs *legs = new proc::MapProcedureLegs;
    legs->ref.airportId = airport.id;
    legs->ref.approachId = approachId;
    legs->ref.transitionId = transitionId;

    while(transitionLegQuery->next())
    {
      legs->transitionLegs.append(buildTransitionLegEntry(airport));
      legs->transitionLegs.last().approachId = approachId;
      legs->transitionLegs.last().transitionId = transitionId;
    }

    // Add a full copy of the approach because approach legs will be modified for different transitions
    proc::MapProcedureLegs *approach = buildApproachLegs(airport, approachId);
    legs->approachLegs = approach->approachLegs;
    legs->runwayEnd = approach->runwayEnd;
    legs->procedureRunway = approach->procedureRunway;
    legs->approachType = approach->approachType;
    legs->approachSuffix = approach->approachSuffix;
    legs->approachFixIdent = approach->approachFixIdent;
    legs->gpsOverlay = approach->gpsOverlay;

    delete approach;

    transitionQuery->bindValue(":id", transitionId);
    transitionQuery->exec();
    if(transitionQuery->next())
    {
      legs->transitionType = transitionQuery->value("type").toString();
      legs->transitionFixIdent = transitionQuery->value("fix_ident").toString();
    }
    transitionQuery->finish();

    postProcessLegs(airport, *legs);

    for(int i = 0; i < legs->size(); ++i)
      transitionLegIndex.insert(legs->at(i).legId, std::make_pair(transitionId, i));

    transitionCache.insert(transitionId, legs);
    return legs;
  }
}

proc::MapProcedureLegs *ProcedureQuery::buildApproachLegs(const map::MapAirport& airport, int approachId)
{
  approachLegQuery->bindValue(":id", approachId);
  approachLegQuery->exec();

  proc::MapProcedureLegs *legs = new proc::MapProcedureLegs;
  legs->ref.airportId = airport.id;
  legs->ref.approachId = approachId;
  legs->ref.transitionId = -1;

  // Load all legs ======================
  while(approachLegQuery->next())
  {
    legs->approachLegs.append(buildApproachLegEntry(airport));
    legs->approachLegs.last().approachId = approachId;
  }

  // Load basic approach information ======================
  approachQuery->bindValue(":id", approachId);
  approachQuery->exec();
  if(approachQuery->next())
  {
    legs->approachType = approachQuery->value("type").toString();
    legs->approachSuffix = approachQuery->value("suffix").toString();
    legs->approachFixIdent = approachQuery->value("fix_ident").toString();
    legs->gpsOverlay = approachQuery->value("has_gps_overlay").toBool();
    legs->procedureRunway = approachQuery->value("runway_name").toString();
  }
  approachQuery->finish();

  // Get all runway ends if they are in the database
  bool runwayFound = false;
  runwayEndIdQuery->bindValue(":id", approachId);
  runwayEndIdQuery->exec();
  if(runwayEndIdQuery->next())
  {
    if(!runwayEndIdQuery->isNull("runway_end_id"))
    {
      legs->runwayEnd = mapQuery->getRunwayEndById(runwayEndIdQuery->value("runway_end_id").toInt());

      // Add altitude to position since it is needed to display the first point in the SID
      legs->runwayEnd.position.setAltitude(airport.getPosition().getAltitude());
      runwayFound = true;
    }
  }
  runwayEndIdQuery->finish();

  if(!runwayFound)
  {
    // Nothing found in the database - search by name fuzzy or add a dummy entry if nothing was found by name
    qWarning() << "Runway end for approach" << approachId << "not found";
    map::MapSearchResult result;
    runwayEndByName(result, legs->procedureRunway, airport);

    if(!result.runwayEnds.isEmpty())
      legs->runwayEnd = result.runwayEnds.first();
  }

  return legs;
}

void ProcedureQuery::postProcessLegs(const map::MapAirport& airport, proc::MapProcedureLegs& legs)
{
  // Update the mapTypes
  assignType(legs);

  updateMagvar(airport, legs);

  // Prepare all leg coordinates
  processLegs(legs);

  // Add artificial legs (not in the database) that end at the runway
  processArtificialLegs(airport, legs);

  // Calculate intercept terminators
  processCourseInterceptLegs(legs);

  processLegsDistanceAndCourse(legs);

  // Correct overlapping conflicting altitude restrictions
  processLegsFixRestrictions(legs);

  updateBounding(legs);

  processLegErrors(legs);

  qDebug() << legs;
}

void ProcedureQuery::processArtificialLegs(const map::MapAirport& airport, proc::MapProcedureLegs& legs)
{
  if(!legs.isEmpty())
  {
    if(legs.mapType & proc::PROCEDURE_DEPARTURE)
    {
      if(legs.runwayEnd.isValid())
      {
        QVector<MapProcedureLeg>& legList = legs.approachLegs.isEmpty() ? legs.transitionLegs : legs.approachLegs;

        if(!legList.isEmpty()) // Add runway fix to departure
        {
          proc::MapProcedureLeg rwleg = createRunwayLeg(legList.first(), legs);
          rwleg.type = proc::DIRECT_TO_RUNWAY;
          rwleg.altRestriction.alt1 = airport.position.getAltitude(); // At 50ft above threshold
          rwleg.line = Line(legs.runwayEnd.position);
          rwleg.mapType = legs.approachLegs.isEmpty() ? proc::PROCEDURE_SID_TRANSITION : proc::PROCEDURE_SID;
          rwleg.distance = 0.f;
          rwleg.course = map::INVALID_COURSE_VALUE;

          legList.prepend(rwleg);
        }
      }
    }
    else
    {
      // if(!legs.first().line.isPoint())
      if(!contains(legs.first().type, {proc::INITIAL_FIX}))
      {
        // Add an artificial initial fix to keep all consistent
        proc::MapProcedureLeg sleg = createStartLeg(legs.first(), legs);
        sleg.type = proc::START_OF_PROCEDURE;
        sleg.line = Line(legs.first().line.getPos1());

        if(legs.mapType & proc::PROCEDURE_STAR_TRANSITION)
        {
          sleg.mapType = proc::PROCEDURE_STAR_TRANSITION;
          legs.approachLegs.prepend(sleg);
        }
        else if(legs.mapType & proc::PROCEDURE_STAR)
        {
          sleg.mapType = proc::PROCEDURE_STAR;
          legs.approachLegs.prepend(sleg);
        }
        else if(legs.mapType & proc::PROCEDURE_TRANSITION)
        {
          sleg.mapType = proc::PROCEDURE_TRANSITION;
          legs.transitionLegs.prepend(sleg);
        }
        else if(legs.mapType & proc::PROCEDURE_APPROACH)
        {
          sleg.mapType = proc::PROCEDURE_APPROACH;
          legs.approachLegs.prepend(sleg);
        }
      }

      for(int i = 0; i < legs.size() - 1; i++)
      {
        // Look for the transition from approach to missed
        proc::MapProcedureLeg& leg = legs[i];
        if(leg.isApproach() && legs.at(i + 1).isMissed())
        {
          if(leg.fixType != "R" && legs.runwayEnd.isValid())
          {
            // Not a runway fix and runway reference is valid - add own runway fix
            proc::MapProcedureLeg rwleg = createRunwayLeg(leg, legs);
            rwleg.type = proc::DIRECT_TO_RUNWAY;
            rwleg.altRestriction.alt1 = airport.position.getAltitude() + 50.f; // At 50ft above threshold
            rwleg.line = Line(leg.line.getPos2(), legs.runwayEnd.position);
            rwleg.mapType = proc::PROCEDURE_APPROACH;

            legs.approachLegs.insert(i + 1 - legs.transitionLegs.size(), rwleg);
            break;
          }
        }
      }
    }
  }
}

void ProcedureQuery::processLegErrors(proc::MapProcedureLegs& legs)
{
  legs.hasError = false;
  for(int i = 1; i < legs.size(); i++)
    legs.hasError |= legs.at(i).hasErrorRef();
}

void ProcedureQuery::processLegsFixRestrictions(proc::MapProcedureLegs& legs)
{
  for(int i = 1; i < legs.size(); i++)
  {
    proc::MapProcedureLeg& leg = legs[i];
    proc::MapProcedureLeg& prevLeg = legs[i - 1];

    if(legs.at(i - 1).isTransition() && legs.at(i).isApproach() && leg.type == proc::INITIAL_FIX &&
       atools::almostEqual(leg.altRestriction.alt1, prevLeg.altRestriction.alt1) &&
       leg.fixIdent == prevLeg.fixIdent)
      // Found the connection between transition and approach with same altitudes
      // Use restriction of the initial fix
      prevLeg.altRestriction.descriptor = leg.altRestriction.descriptor;
  }
}

void ProcedureQuery::processLegsDistanceAndCourse(proc::MapProcedureLegs& legs)
{
  legs.transitionDistance = 0.f;
  legs.approachDistance = 0.f;
  legs.missedDistance = 0.f;

  for(int i = 0; i < legs.size(); i++)
  {
    proc::MapProcedureLeg& leg = legs[i];
    const proc::MapProcedureLeg *prevLeg = i > 0 ? &legs[i - 1] : nullptr;
    proc::ProcedureLegType type = leg.type;

    if(!leg.line.isValid())
      qWarning() << "leg line for leg is invalid" << leg;

    // ===========================================================
    else if(type == proc::INITIAL_FIX || type == proc::START_OF_PROCEDURE)
    {
      leg.calculatedDistance = 0.f;
      leg.calculatedTrueCourse = map::INVALID_COURSE_VALUE;
      leg.geometry << leg.line.getPos1() << leg.line.getPos2();
    }
    else if(contains(type, {proc::ARC_TO_FIX, proc::CONSTANT_RADIUS_ARC}))
    {
      atools::geo::calcArcLength(leg.line, leg.recFixPos, leg.turnDirection == "L", &leg.calculatedDistance,
                                 &leg.geometry);
      leg.calculatedDistance = meterToNm(leg.calculatedDistance);
      leg.calculatedTrueCourse = map::INVALID_COURSE_VALUE;
    }
    // ===========================================================
    else if(type == proc::COURSE_TO_FIX)
    {
      if(leg.interceptPos.isValid())
      {
        leg.calculatedDistance = meterToNm(leg.line.getPos1().distanceMeterTo(leg.interceptPos) +
                                           leg.interceptPos.distanceMeterTo(leg.line.getPos2()));
        leg.calculatedTrueCourse = normalizeCourse(leg.interceptPos.angleDegTo(leg.line.getPos2()));
        leg.geometry << leg.line.getPos1() << leg.interceptPos << leg.line.getPos2();
      }
      else
      {
        leg.calculatedDistance = meterToNm(leg.line.lengthMeter());
        leg.calculatedTrueCourse = normalizeCourse(leg.line.angleDeg());
        leg.geometry << leg.line.getPos1() << leg.line.getPos2();
      }
    }
    // ===========================================================
    else if(type == proc::PROCEDURE_TURN)
    {
      // Distance is towards turn point
      leg.calculatedDistance = meterToNm(leg.line.getPos1().distanceMeterTo(leg.procedureTurnPos));

      // if(nextLeg != nullptr)
      // leg.calculatedDistance += meterToNm(leg.procedureTurnPos.distanceMeterTo(nextLeg->line.getPos1()));

      // Course from fix to turn point
      leg.calculatedTrueCourse = normalizeCourse(leg.course + (leg.turnDirection == "L" ? -45.f : 45.f) + leg.magvar);

      leg.geometry << leg.line.getPos1() << leg.procedureTurnPos;
    }
    // ===========================================================
    else if(contains(type, {proc::COURSE_TO_ALTITUDE, proc::FIX_TO_ALTITUDE,
                            proc::HEADING_TO_ALTITUDE_TERMINATION,
                            proc::FROM_FIX_TO_MANUAL_TERMINATION, proc::HEADING_TO_MANUAL_TERMINATION}))
    {
      leg.calculatedDistance = 2.f;
      leg.calculatedTrueCourse = normalizeCourse(leg.line.angleDeg());
      leg.geometry << leg.line.getPos1() << leg.line.getPos2();
    }
    // ===========================================================
    else if(type == proc::TRACK_FROM_FIX_FROM_DISTANCE)
    {
      leg.calculatedDistance = leg.distance;
      leg.calculatedTrueCourse = normalizeCourse(leg.line.angleDeg());
      leg.geometry << leg.line.getPos1() << leg.line.getPos2();
    }
    // ===========================================================
    else if(contains(type, {proc::HOLD_TO_MANUAL_TERMINATION, proc::HOLD_TO_FIX, proc::HOLD_TO_ALTITUDE}))
    {
      leg.calculatedDistance = meterToNm(leg.line.lengthMeter());
      leg.calculatedTrueCourse = leg.legTrueCourse();
      leg.geometry << leg.line.getPos1() << leg.line.getPos2();

      float segmentLength;
      if(leg.time > 0.f)
        // 3.5 nm per minute
        segmentLength = leg.time * 3.5f;
      else if(leg.distance > 0.f)
        segmentLength = leg.distance;
      else
        segmentLength = 3.5f;

      leg.holdLine.setPos2(leg.line.getPos1());
      leg.holdLine.setPos1(leg.line.getPos1().endpoint(nmToMeter(segmentLength),
                                                       opposedCourseDeg(leg.calculatedTrueCourse)).normalize());
    }
    else if(contains(type, {proc::TRACK_FROM_FIX_TO_DME_DISTANCE, proc::COURSE_TO_DME_DISTANCE,
                            proc::HEADING_TO_DME_DISTANCE_TERMINATION,
                            proc::COURSE_TO_RADIAL_TERMINATION, proc::HEADING_TO_RADIAL_TERMINATION,
                            proc::DIRECT_TO_FIX, proc::TRACK_TO_FIX,
                            proc::COURSE_TO_INTERCEPT, proc::HEADING_TO_INTERCEPT,
                            proc::DIRECT_TO_RUNWAY}))
    {
      leg.calculatedDistance = meterToNm(leg.line.lengthMeter());
      leg.calculatedTrueCourse = normalizeCourse(leg.line.angleDeg());
      leg.geometry << leg.line.getPos1() << leg.line.getPos2();
    }

    if(prevLeg != nullptr && !leg.intercept && type != proc::INITIAL_FIX)
      // Add distance from any existing gaps, bows or turns except for intercept legs
      leg.calculatedDistance += meterToNm(prevLeg->line.getPos2().distanceMeterTo(leg.line.getPos1()));

    if(leg.calculatedDistance >= map::INVALID_DISTANCE_VALUE / 2)
      leg.calculatedDistance = 0.f;
    if(leg.calculatedTrueCourse >= map::INVALID_DISTANCE_VALUE / 2)
      leg.calculatedTrueCourse = map::INVALID_COURSE_VALUE;

    if(leg.isTransition() || leg.isSidTransition() || leg.isStarTransition())
      legs.transitionDistance += leg.calculatedDistance;

    if(leg.isApproach() || leg.isStar() || leg.isSid())
      legs.approachDistance += leg.calculatedDistance;

    if(leg.isMissed())
      legs.missedDistance += leg.calculatedDistance;
  }
}

void ProcedureQuery::processLegs(proc::MapProcedureLegs& legs)
{
  // Assumptions: 3.5 nm per min
  // Climb 500 ft/min
  // Intercept 30 for localizers and 30-45 for others
  Pos lastPos;
  for(int i = 0; i < legs.size(); ++i)
  {
    if(legs.mapType & proc::PROCEDURE_DEPARTURE && i == 0)
      lastPos = legs.runwayEnd.position;

    Pos curPos;
    proc::MapProcedureLeg& leg = legs[i];
    proc::ProcedureLegType type = leg.type;

    // ===========================================================
    if(type == proc::ARC_TO_FIX)
    {
      curPos = leg.fixPos;

      leg.displayText << leg.recFixIdent + "/" + Unit::distNm(leg.rho, true, 20, true) + "/" +
      QLocale().toString(leg.theta, 'f', 0) + tr("°M");
      leg.remarks << tr("DME %1").arg(Unit::distNm(leg.rho, true, 20, true));
    }
    // ===========================================================
    else if(type == proc::COURSE_TO_FIX)
    {
      // Calculate the leading extended position to the fix
      Pos extended = leg.fixPos.endpoint(nmToMeter(leg.distance),
                                         opposedCourseDeg(leg.legTrueCourse())).normalize();
      atools::geo::LineDistance result;
      lastPos.distanceMeterToLine(extended, leg.fixPos, result);

      // Check if lines overlap - if not calculate an intercept position
      if(std::abs(result.distance) > nmToMeter(1.f))
      {
        // Extended position leading towards the fix which is far away from last fix - calculate an intercept position
        float crs = leg.legTrueCourse();

        // Try left or right intercept
        Pos intr1 = Pos::intersectingRadials(extended, crs, lastPos, crs - 45.f).normalize();
        Pos intr2 = Pos::intersectingRadials(extended, crs, lastPos, crs + 45.f).normalize();
        Pos intersect;

        // Use whatever course is shorter - calculate distance to interception candidates
        float dist1 = intr1.distanceMeterTo(lastPos);
        float dist2 = intr2.distanceMeterTo(lastPos);
        if(dist1 < dist2)
          intersect = intr1;
        else
          intersect = intr2;

        if(intersect.isValid())
        {
          intersect.distanceMeterToLine(extended, leg.fixPos, result);

          if(result.status == atools::geo::ALONG_TRACK)
          {
            // Leg intercepted - set point for drawing
            leg.interceptPos = intersect;
          }
          else if(result.status == atools::geo::AFTER_END)
          {
            // Fly to fix - end of leg

            if(contains(leg.turnDirection, {"L", "R"}))
            {
              float extDist = extended.distanceMeterTo(lastPos);
              if(extDist > nmToMeter(1.f))
                // Draw large bow to extended postition or allow intercept of leg
                lastPos = extended;
            }
            // else turn
          }
          else if(result.status == atools::geo::BEFORE_START)
          {
            // Fly to start of leg
            lastPos = extended;
          }
          else
            qWarning() << "leg line type" << leg.type << "fix" << leg.fixIdent
                       << "invalid cross track"
                       << "approachId" << leg.approachId
                       << "transitionId" << leg.transitionId << "legId" << leg.legId;
        }
        else
        {
          // No intercept point in reasonable distance found
          float extDist = extended.distanceMeterTo(lastPos);
          if(extDist > nmToMeter(1.f))
            // Draw large bow to extended postition or allow intercept of leg
            lastPos = extended;
        }
      }
      if(leg.interceptPos.isValid())
      {
        // Add intercept for display
        leg.displayText << tr("Intercept");
        leg.displayText << tr("Course to Fix");
      }

      curPos = leg.fixPos;
    }
    // ===========================================================
    else if(contains(type, {proc::DIRECT_TO_FIX, proc::INITIAL_FIX, proc::START_OF_PROCEDURE,
                            proc::TRACK_TO_FIX, proc::CONSTANT_RADIUS_ARC}))
    {
      curPos = leg.fixPos;
    }
    // ===========================================================
    else if(type == proc::PROCEDURE_TURN)
    {
      float course;
      if(leg.turnDirection == "L")
        // Turn right and then turn 180 deg left
        course = leg.legTrueCourse() - 45.f;
      else
        // Turn left and then turn 180 deg right
        course = leg.legTrueCourse() + 45.f;

      leg.procedureTurnPos = leg.fixPos.endpoint(nmToMeter(leg.distance), course).normalize();
      lastPos = leg.fixPos;
      curPos = leg.procedureTurnPos;
    }
    // ===========================================================
    else if(contains(type, {proc::COURSE_TO_ALTITUDE, proc::FIX_TO_ALTITUDE,
                            proc::HEADING_TO_ALTITUDE_TERMINATION}))
    {
      // TODO calculate distance by altitude
      Pos start = lastPos.isValid() ? lastPos : leg.fixPos;

      if(!start.isValid() && legs.mapType & proc::PROCEDURE_DEPARTURE && legs.runwayEnd.isValid())
        start = legs.runwayEnd.position;

      if(!lastPos.isValid())
        lastPos = start;
      curPos = start.endpoint(nmToMeter(2.f), leg.legTrueCourse()).normalize();
      leg.displayText << tr("Altitude");
    }
    // ===========================================================
    else if(contains(type, {proc::COURSE_TO_RADIAL_TERMINATION, proc::HEADING_TO_RADIAL_TERMINATION}))
    {
      Pos start = lastPos.isValid() ? lastPos : leg.fixPos;
      if(!lastPos.isValid())
        lastPos = start;

      Pos center = leg.recFixPos.isValid() ? leg.recFixPos : leg.fixPos;

      Pos intersect = Pos::intersectingRadials(start, leg.legTrueCourse(), center, leg.theta + leg.magvar);

      if(intersect.isValid())
        curPos = intersect;
      else
      {
        curPos = center;
        qWarning() << "leg line type" << type << "fix" << leg.fixIdent << "no intersectingRadials found"
                   << "approachId" << leg.approachId << "transitionId" << leg.transitionId << "legId" << leg.legId;
      }
      leg.displayText << leg.recFixIdent + "/" + QLocale().toString(leg.theta, 'f', 0);
    }
    // ===========================================================
    else if(type == proc::TRACK_FROM_FIX_FROM_DISTANCE)
    {
      if(!lastPos.isValid())
        lastPos = leg.fixPos;

      curPos = leg.fixPos.endpoint(nmToMeter(leg.distance), leg.legTrueCourse()).normalize();

      leg.displayText << leg.fixIdent + "/" + Unit::distNm(leg.distance, true, 20, true) + "/" +
      QLocale().toString(leg.course, 'f', 0) + (leg.trueCourse ? tr("°T") : tr("°M"));
    }
    // ===========================================================
    else if(contains(type, {proc::TRACK_FROM_FIX_TO_DME_DISTANCE, proc::COURSE_TO_DME_DISTANCE,
                            proc::HEADING_TO_DME_DISTANCE_TERMINATION}))
    {
      Pos start = lastPos.isValid() ? lastPos : (leg.fixPos.isValid() ? leg.fixPos : leg.recFixPos);

      Pos center = leg.recFixPos.isValid() ? leg.recFixPos : leg.fixPos;
      Line line(start, start.endpoint(nmToMeter(leg.distance * 2), leg.legTrueCourse()).normalize());

      if(!lastPos.isValid())
        lastPos = start;
      Pos intersect = line.intersectionWithCircle(center, nmToMeter(leg.distance), 10.f);

      if(intersect.isValid())
        curPos = intersect;
      else
      {
        curPos = center;
        qWarning() << "leg line type" << type << "fix" << leg.fixIdent << "no intersectionWithCircle found"
                   << "approachId" << leg.approachId << "transitionId" << leg.transitionId << "legId" << leg.legId;
      }

      leg.displayText << leg.recFixIdent + "/" + Unit::distNm(leg.distance, true, 20, true) + "/" +
      QLocale().toString(leg.course, 'f', 0) + (leg.trueCourse ? tr("°T") : tr("°M"));
    }
    // ===========================================================
    else if(contains(type, {proc::FROM_FIX_TO_MANUAL_TERMINATION, proc::HEADING_TO_MANUAL_TERMINATION}))
    {
      if(!lastPos.isValid())
        lastPos = leg.fixPos;

      if(leg.fixPos.isValid())
        curPos = leg.fixPos.endpoint(nmToMeter(3.f), leg.legTrueCourse()).normalize();
      else
        curPos = lastPos.endpoint(nmToMeter(3.f), leg.legTrueCourse()).normalize();

      leg.displayText << tr("Manual");
    }
    // ===========================================================
    else if(type == proc::HOLD_TO_ALTITUDE)
    {
      curPos = leg.fixPos;
      leg.displayText << tr("Altitude");
    }
    // ===========================================================
    else if(type == proc::HOLD_TO_FIX)
    {
      curPos = leg.fixPos;
      leg.displayText << tr("Single");
    }
    // ===========================================================
    else if(type == proc::HOLD_TO_MANUAL_TERMINATION)
    {
      curPos = leg.fixPos;
      leg.displayText << tr("Manual");
    }

    if(legs.mapType & proc::PROCEDURE_DEPARTURE && i == 0)
      // First leg of a SID start at runway end
      leg.line = Line(legs.runwayEnd.position, curPos);
    else
      leg.line = Line(lastPos.isValid() ? lastPos : curPos, curPos);

    if(!leg.line.isValid())
      qWarning() << "leg line type" << type << "fix" << leg.fixIdent << "invalid line"
                 << "approachId" << leg.approachId << "transitionId" << leg.transitionId << "legId" << leg.legId;
    lastPos = curPos;
  }
}

void ProcedureQuery::processCourseInterceptLegs(proc::MapProcedureLegs& legs)
{
  for(int i = 0; i < legs.size(); ++i)
  {
    proc::MapProcedureLeg& leg = legs[i];
    proc::MapProcedureLeg *prevLeg = i > 0 ? &legs[i - 1] : nullptr;
    proc::MapProcedureLeg *nextLeg = i < legs.size() - 1 ? &legs[i + 1] : nullptr;
    proc::MapProcedureLeg *secondNextLeg = i < legs.size() - 2 ? &legs[i + 2] : nullptr;

    if(contains(leg.type, {proc::COURSE_TO_INTERCEPT, proc::HEADING_TO_INTERCEPT}))
    {
      if(nextLeg != nullptr)
      {
        proc::MapProcedureLeg *next = nextLeg->type == proc::INITIAL_FIX ? secondNextLeg : nextLeg;

        if(nextLeg->type == proc::INITIAL_FIX)
          // Do not show the cut-off initial fix
          nextLeg->disabled = true;

        if(next != nullptr)
        {
          bool nextIsArc = next->isCircular();
          Pos start = prevLeg != nullptr ? prevLeg->line.getPos2() : leg.fixPos;
          Pos intersect;
          if(nextIsArc)
          {
            Line line(start, start.endpoint(nmToMeter(200), leg.legTrueCourse()).normalize());
            intersect = line.intersectionWithCircle(next->recFixPos, nmToMeter(next->rho), 20);
          }
          else
            intersect = Pos::intersectingRadials(start, leg.legTrueCourse(), next->line.getPos1(), next->legTrueCourse());

          leg.line.setPos1(start);

          if(intersect.isValid() && intersect.distanceMeterTo(start) < nmToMeter(200.f))
          {
            atools::geo::LineDistance result;

            next->line.distanceMeterToLine(intersect, result);

            if(result.status == atools::geo::ALONG_TRACK)
            {
              // Intercepting the next leg
              next->intercept = true;
              next->line.setPos1(intersect);

              leg.line.setPos2(intersect);
              leg.displayText << tr("Intercept");

              if(nextIsArc)
                leg.displayText << next->recFixIdent + "/" + Unit::distNm(next->rho, true, 20, true);
              else
                leg.displayText << tr("Leg");
            }
            else if(result.status == atools::geo::BEFORE_START)
            {
              // Link directly to start of next leg
              leg.line.setPos2(next->line.getPos2());
            }
            else if(result.status == atools::geo::AFTER_END)
            {
              // Link directly to end of next leg
              next->intercept = true;
              leg.line.setPos2(next->line.getPos2());
              next->line.setPos1(next->line.getPos2());
              next->line.setPos2(next->line.getPos2());
              leg.displayText << tr("Intercept");
            }
            else
              qWarning() << "leg line type" << leg.type << "fix" << leg.fixIdent
                         << "invalid cross track"
                         << "approachId" << leg.approachId
                         << "transitionId" << leg.transitionId << "legId" << leg.legId;

          }
          else
          {
            qWarning() << "leg line type" << leg.type << "fix" << leg.fixIdent
                       << "no intersectingRadials/intersectionWithCircle found"
                       << "approachId" << leg.approachId << "transitionId" << leg.transitionId << "legId" << leg.legId;
            leg.displayText << tr("Intercept") << tr("Leg");
            leg.line.setPos2(next->line.getPos1());
          }
        }
      }
    }
  }
}

void ProcedureQuery::initQueries()
{
  deInitQueries();

  approachLegQuery = new SqlQuery(db);
  approachLegQuery->prepare("select * from approach_leg where approach_id = :id "
                            "order by approach_leg_id");

  transitionLegQuery = new SqlQuery(db);
  transitionLegQuery->prepare("select * from transition_leg where transition_id = :id "
                              "order by transition_leg_id");

  transitionIdForLegQuery = new SqlQuery(db);
  transitionIdForLegQuery->prepare("select transition_id as id from transition_leg where transition_leg_id = :id");

  approachIdForTransQuery = new SqlQuery(db);
  approachIdForTransQuery->prepare("select approach_id from transition where transition_id = :id");

  runwayEndIdQuery = new SqlQuery(db);
  runwayEndIdQuery->prepare("select e.runway_end_id from approach a "
                            "join runway_end e on a.runway_end_id = e.runway_end_id where approach_id = :id");

  transitionQuery = new SqlQuery(db);
  transitionQuery->prepare("select type, fix_ident from transition where transition_id = :id");

  approachQuery = new SqlQuery(db);
  approachQuery->prepare("select type, suffix, has_gps_overlay, fix_ident, runway_name "
                         "from approach where approach_id = :id");

  approachIdByNameQuery = new SqlQuery(db);
  approachIdByNameQuery->prepare("select approach_id, suffix , runway_name from approach "
                                 "where fix_ident = :fixident and type = :type and airport_id = :apid");

  transitionIdByNameQuery = new SqlQuery(db);
  transitionIdByNameQuery->prepare("select transition_id from transition where fix_ident = :fixident and "
                                   "type = :type and approach_id = :apprid");

  transitionIdsForApproachQuery = new SqlQuery(db);
  transitionIdsForApproachQuery->prepare("select transition_id from transition where approach_id = :id");
}

void ProcedureQuery::deInitQueries()
{
  approachCache.clear();
  transitionCache.clear();
  approachLegIndex.clear();
  transitionLegIndex.clear();

  delete approachLegQuery;
  approachLegQuery = nullptr;

  delete transitionLegQuery;
  transitionLegQuery = nullptr;

  delete transitionIdForLegQuery;
  transitionIdForLegQuery = nullptr;

  delete approachIdForTransQuery;
  approachIdForTransQuery = nullptr;

  delete runwayEndIdQuery;
  runwayEndIdQuery = nullptr;

  delete transitionQuery;
  transitionQuery = nullptr;

  delete approachQuery;
  approachQuery = nullptr;

  delete approachIdByNameQuery;
  approachIdByNameQuery = nullptr;

  delete transitionIdByNameQuery;
  transitionIdByNameQuery = nullptr;

  delete transitionIdsForApproachQuery;
  transitionIdsForApproachQuery = nullptr;
}

void ProcedureQuery::clearFlightplanProcedureProperties(QHash<QString, QString>& properties,
                                                        const proc::MapProcedureTypes& type)
{
  if(type & proc::PROCEDURE_SID)
  {
    properties.remove("sidappr");
    properties.remove("sidapprrw");
    properties.remove("sidapprdistance");
    properties.remove("sidapprsize");
  }

  if(type & proc::PROCEDURE_SID_TRANSITION)
  {
    properties.remove("sidtrans");
    properties.remove("sidtransdistance");
    properties.remove("sidtranssize");
  }

  if(type & proc::PROCEDURE_STAR)
  {
    properties.remove("star");
    properties.remove("stardistance");
    properties.remove("starsize");
  }

  if(type & proc::PROCEDURE_STAR_TRANSITION)
  {
    properties.remove("startrans");
    properties.remove("startransdistance");
    properties.remove("startranssize");
  }

  if(type & proc::PROCEDURE_TRANSITION)
  {
    properties.remove("transition");
    properties.remove("transitiontype");
    properties.remove("transitiondistance");
    properties.remove("transitionsize");
  }

  if(type & proc::PROCEDURE_APPROACH)
  {
    properties.remove("approach");
    properties.remove("approachtype");
    properties.remove("approachrw");
    properties.remove("approachsuffix");
    properties.remove("approachdistance");
    properties.remove("approachsize");
  }
}

void ProcedureQuery::extractLegsForFlightplanProperties(QHash<QString, QString>& properties,
                                                        const proc::MapProcedureLegs& arrivalLegs,
                                                        const proc::MapProcedureLegs& starLegs,
                                                        const proc::MapProcedureLegs& departureLegs)
{
  if(!departureLegs.isEmpty())
  {
    if(!departureLegs.transitionFixIdent.isEmpty())
    {
      properties.insert("sidtrans", departureLegs.transitionFixIdent);
      properties.insert("sidtransdistance", QString::number(departureLegs.transitionDistance, 'f', 1));
      properties.insert("sidtranssize", QString::number(departureLegs.transitionLegs.size()));
    }
    if(!departureLegs.approachFixIdent.isEmpty())
    {
      properties.insert("sidappr", departureLegs.approachFixIdent);
      properties.insert("sidapprrw", departureLegs.procedureRunway);
      properties.insert("sidapprdistance", QString::number(departureLegs.approachDistance, 'f', 1));
      properties.insert("sidapprsize", QString::number(departureLegs.approachLegs.size()));
    }
  }

  if(!starLegs.isEmpty())
  {
    if(!starLegs.transitionFixIdent.isEmpty())
    {
      properties.insert("startrans", starLegs.transitionFixIdent);
      properties.insert("startransdistance", QString::number(starLegs.transitionDistance, 'f', 1));
      properties.insert("startranssize", QString::number(starLegs.transitionLegs.size()));
    }
    if(!starLegs.isEmpty() && !starLegs.approachFixIdent.isEmpty())
    {
      properties.insert("star", starLegs.approachFixIdent);
      properties.insert("stardistance", QString::number(starLegs.approachDistance, 'f', 1));
      properties.insert("starsize", QString::number(starLegs.approachLegs.size()));
    }
  }

  if(!arrivalLegs.isEmpty())
  {
    if(!arrivalLegs.transitionFixIdent.isEmpty())
    {
      properties.insert("transition", arrivalLegs.transitionFixIdent);
      properties.insert("transitiontype", arrivalLegs.transitionType);
      properties.insert("transitiondistance", QString::number(arrivalLegs.transitionDistance, 'f', 1));
      properties.insert("transitionsize", QString::number(arrivalLegs.transitionLegs.size()));
    }

    if(!arrivalLegs.approachFixIdent.isEmpty())
    {
      properties.insert("approach", arrivalLegs.approachFixIdent);
      properties.insert("approachtype", arrivalLegs.approachType);
      properties.insert("approachrw", arrivalLegs.procedureRunway);
      properties.insert("approachsuffix", arrivalLegs.approachSuffix);
      properties.insert("approachdistance", QString::number(arrivalLegs.approachDistance, 'f', 1));
      properties.insert("approachsize", QString::number(arrivalLegs.approachLegs.size()));
    }
  }
}

int ProcedureQuery::getSidId(const map::MapAirport& departure, const QString& sid,
                             const QString& runway, float distance, int size)
{
  int sidApprId = -1;
  // Get a SID id =================================================================
  if(!sid.isEmpty())
  {
    approachIdByNameQuery->bindValue(":fixident", sid);
    approachIdByNameQuery->bindValue(":type", "GPS");
    approachIdByNameQuery->bindValue(":apid", departure.id);

    sidApprId = findApproachId(departure, approachIdByNameQuery, "D", runway, distance, size);
    if(sidApprId == -1)
      qWarning() << "Loading of SID" << sid << "failed";
  }
  return sidApprId;
}

int ProcedureQuery::getSidTransitionId(const map::MapAirport& departure, const QString& sidTrans,
                                       int sidId, float distance, int size)
{
  int sidTransId = -1;
  // Get a SID transition id =================================================================
  if(!sidTrans.isEmpty() && sidId != -1)
  {
    transitionIdByNameQuery->bindValue(":fixident", sidTrans);
    transitionIdByNameQuery->bindValue(":type", "F");
    transitionIdByNameQuery->bindValue(":apprid", sidId);

    sidTransId = findTransitionId(departure, transitionIdByNameQuery, distance, size);
    if(sidTransId == -1)
      qWarning() << "Loading of SID transition" << sidTrans << "failed";
  }

  return sidTransId;
}

int ProcedureQuery::getStarId(const map::MapAirport& destination, const QString& star,
                              float distance, int size)
{
  int starId = -1;
  // Get a STAR id =================================================================
  if(!star.isEmpty())
  {
    approachIdByNameQuery->bindValue(":fixident", star);
    approachIdByNameQuery->bindValue(":type", "GPS");
    approachIdByNameQuery->bindValue(":apid", destination.id);

    starId = findApproachId(destination, approachIdByNameQuery, "A", QString(), /*No runway*/ distance, size);
    if(starId == -1)
    {
      qWarning() << "Loading of STAR" << star << "failed";
    }
  }
  return starId;
}

int ProcedureQuery::getStarTransitionId(const map::MapAirport& destination, const QString& starTrans, int starId,
                                        float distance, int size)
{
  int starTransId = -1;
  // Get a STAR transition id =================================================================
  if(!starTrans.isEmpty() && starId != -1)
  {
    transitionIdByNameQuery->bindValue(":fixident", starTrans);
    transitionIdByNameQuery->bindValue(":type", "F");
    transitionIdByNameQuery->bindValue(":apprid", starId);

    starTransId = findTransitionId(destination, transitionIdByNameQuery, distance, size);
    if(starTransId == -1)
      qWarning() << "Loading of STAR transition" << starTrans << "failed";
  }
  return starTransId;
}

void ProcedureQuery::clearCache()
{
  qDebug() << Q_FUNC_INFO;

  approachCache.clear();
  transitionCache.clear();
  approachLegIndex.clear();
  transitionLegIndex.clear();
}

QVector<int> ProcedureQuery::getTransitionIdsForApproach(int approachId)
{
  QVector<int> transitionIds;

  transitionIdsForApproachQuery->bindValue(":id", approachId);
  transitionIdsForApproachQuery->exec();

  while(transitionIdsForApproachQuery->next())
    transitionIds.append(transitionIdsForApproachQuery->value("transition_id").toInt());
  return transitionIds;
}

bool ProcedureQuery::getLegsForFlightplanProperties(const QHash<QString, QString> properties,
                                                    const map::MapAirport& departure,
                                                    const map::MapAirport& destination,
                                                    proc::MapProcedureLegs& arrivalLegs,
                                                    proc::MapProcedureLegs& starLegs,
                                                    proc::MapProcedureLegs& departureLegs)
{
  bool error = false;

  int sidApprId = -1, sidTransId = -1, approachId = -1, starId = -1, starTransId = -1, transitionId = -1;
  // Get a SID id (approach and transition) =================================================================
  // Get a SID id =================================================================
  if(properties.contains("sidappr"))
  {
    sidApprId = getSidId(departure,
                         properties.value("sidappr"),
                         properties.value("sidapprrw"),
                         properties.value("sidapprdistance").toFloat(),
                         properties.value("sidapprsize").toInt());
    if(sidApprId == -1)
    {
      qWarning() << "Loading of approach" << properties.value("sidappr") << "failed";
      error = true;
    }
  }

  // Get a SID transition id =================================================================
  if(properties.contains("sidtrans") && sidApprId != -1)
  {
    sidTransId = getSidTransitionId(departure,
                                    properties.value("sidtrans"),
                                    sidApprId,
                                    properties.value("sidtransdistance").toFloat(),
                                    properties.value("sidtranssize").toInt());
    if(sidTransId == -1)
    {
      qWarning() << "Loading of approach" << properties.value("sidtrans") << "failed";
      error = true;
    }
  }

  // Get an approach id =================================================================
  if(properties.contains("approach"))
  {
    approachIdByNameQuery->bindValue(":fixident", properties.value("approach"));
    approachIdByNameQuery->bindValue(":type", properties.value("approachtype"));
    approachIdByNameQuery->bindValue(":apid", destination.id);

    approachId = findApproachId(destination, approachIdByNameQuery,
                                properties.value("approachsuffix"),
                                properties.value("approachrw"),
                                properties.value("approachdistance").toFloat(),
                                properties.value("approachsize").toInt());
    if(approachId == -1)
    {
      qWarning() << "Loading of approach" << properties.value("approach") << "failed";
      error = true;
    }
  }

  // Get a transition id =================================================================
  if(properties.contains("transition") && approachId != -1)
  {
    transitionIdByNameQuery->bindValue(":fixident", properties.value("transition"));
    transitionIdByNameQuery->bindValue(":type", properties.value("transitiontype"));
    transitionIdByNameQuery->bindValue(":apprid", approachId);

    transitionId = findTransitionId(destination, transitionIdByNameQuery,
                                    properties.value("transitiondistance").toFloat(),
                                    properties.value("transitionsize").toInt());
    if(transitionId == -1)
    {
      qWarning() << "Loading of transition" << properties.value("transition") << "failed";
      error = true;
    }
  }

  // Get a STAR id =================================================================
  if(properties.contains("star"))
    starId = getStarId(destination,
                       properties.value("star"),
                       properties.value("stardistance").toFloat(),
                       properties.value("starsize").toInt());

  // Get a STAR transition id =================================================================
  if(properties.contains("startrans") && starId != -1)
    starTransId = getStarTransitionId(destination,
                                      properties.value("startrans"),
                                      starId,
                                      properties.value("startransdistance").toFloat(),
                                      properties.value("startranssize").toInt());

  if(!error) // load all or nothing in case of error
  {
    if(sidTransId != -1) // Fetch and copy SID and transition together (here from cache)
    {
      const proc::MapProcedureLegs *legs = getTransitionLegs(departure, sidTransId);
      if(legs != nullptr)
        departureLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << departure.id << sidTransId;
    }
    else if(sidApprId != -1) // Fetch and copy SID only from cache
    {
      const proc::MapProcedureLegs *legs = getApproachLegs(departure, sidApprId);
      if(legs != nullptr)
        departureLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << departure.id << sidApprId;
    }

    if(transitionId != -1) // Fetch and copy transition together with approach (here from cache)
    {
      const proc::MapProcedureLegs *legs = getTransitionLegs(destination, transitionId);
      if(legs != nullptr)
        arrivalLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << destination.id << transitionId;
    }
    else if(approachId != -1) // Fetch and copy approach only from cache
    {
      const proc::MapProcedureLegs *legs = getApproachLegs(destination, approachId);
      if(legs != nullptr)
        arrivalLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << destination.id << approachId;
    }

    if(starTransId != -1)
    {
      const proc::MapProcedureLegs *legs = getTransitionLegs(destination, starTransId);
      if(legs != nullptr)
        starLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << destination.id << starTransId;
    }
    else if(starId != -1)
    {
      const proc::MapProcedureLegs *legs = getApproachLegs(destination, starId);
      if(legs != nullptr)
        starLegs = *legs;
      else
        qWarning() << Q_FUNC_INFO << "legs not found for" << destination.id << starId;
    }
  }
  return !error;
}

QString ProcedureQuery::getSidAndTransition(QHash<QString, QString>& properties)
{
  QString retval;
  if(properties.contains("sidappr"))
    retval += properties.value("sidappr");

  if(properties.contains("sidtrans"))
    retval += "." + properties.value("sidtrans");
  return retval;
}

QString ProcedureQuery::getStarAndTransition(QHash<QString, QString>& properties)
{
  QString retval;
  if(properties.contains("star"))
    retval += properties.value("star");

  if(properties.contains("startrans"))
    retval += "." + properties.value("startrans");
  return retval;
}

int ProcedureQuery::findTransitionId(const map::MapAirport& airport, atools::sql::SqlQuery *query,
                                     float distance, int size)
{
  return findProcedureLegId(airport, query, QString(), QString(), distance, size, true);
}

int ProcedureQuery::findApproachId(const map::MapAirport& airport, atools::sql::SqlQuery *query,
                                   const QString& suffix, const QString& runway, float distance, int size)
{
  return findProcedureLegId(airport, query, suffix, runway, distance, size, false);
}

int ProcedureQuery::findProcedureLegId(const map::MapAirport& airport, atools::sql::SqlQuery *query,
                                       const QString& suffix, const QString& runway,
                                       float distance, int size, bool transition)
{
  int procedureId = -1;
  query->exec();
  QVector<int> ids;
  while(query->next())
  {
    // Compare the suffix manually since the ifnull function makes the query unstable (did not work with undo)
    if(!transition && (suffix != query->value("suffix").toString() ||
                       // Runway will be compared directly to the approach and not the airport runway
                       (!runway.isEmpty() && runway != query->value("runway_name").toString())))
      continue;

    ids.append(query->value(transition ? "transition_id" : "approach_id").toInt());
  }
  query->finish();

  if(ids.size() == 1)
    // Found exactly one - no need to compare distance and leg number
    procedureId = ids.first();
  else
  {
    for(int id : ids)
    {
      const proc::MapProcedureLegs *legs = nullptr;

      if(transition)
        legs = getTransitionLegs(airport, id);
      else
        legs = getApproachLegs(airport, id);

      float legdist = transition ? legs->transitionDistance : legs->approachDistance;
      int legsize = transition ? legs->transitionLegs.size() : legs->approachLegs.size();

      if(legs != nullptr &&
         (!(distance < map::INVALID_DISTANCE_VALUE) || atools::almostEqual(legdist, distance, 0.2f)) &&
         (size == -1 || size == legsize))
      {
        procedureId = id;
        break;
      }
    }
  }

  return procedureId;
}

void ProcedureQuery::assignType(proc::MapProcedureLegs& procedure)
{
  if(NavApp::hasSidStarInDatabase() && procedure.approachType == "GPS" &&
     (procedure.approachSuffix == "A" || procedure.approachSuffix == "D") && procedure.gpsOverlay)
  {
    if(procedure.approachSuffix == "A")
    {
      if(!procedure.approachLegs.isEmpty())
      {
        procedure.mapType = proc::PROCEDURE_STAR;
        for(MapProcedureLeg& leg : procedure.approachLegs)
          leg.mapType = proc::PROCEDURE_STAR;
      }

      if(!procedure.transitionLegs.isEmpty())
      {
        procedure.mapType |= proc::PROCEDURE_STAR_TRANSITION;
        for(MapProcedureLeg& leg : procedure.transitionLegs)
          leg.mapType = proc::PROCEDURE_STAR_TRANSITION;
      }
    }
    else if(procedure.approachSuffix == "D")
    {
      if(!procedure.approachLegs.isEmpty())
      {
        procedure.mapType = proc::PROCEDURE_SID;
        for(MapProcedureLeg& leg : procedure.approachLegs)
          leg.mapType = proc::PROCEDURE_SID;
      }

      if(!procedure.transitionLegs.isEmpty())
      {
        procedure.mapType |= proc::PROCEDURE_SID_TRANSITION;
        for(MapProcedureLeg& leg : procedure.transitionLegs)
          leg.mapType = proc::PROCEDURE_SID_TRANSITION;
      }
    }
  }
  else
  {
    if(!procedure.approachLegs.isEmpty())
    {
      procedure.mapType = proc::PROCEDURE_APPROACH;
      for(MapProcedureLeg& leg : procedure.approachLegs)
        leg.mapType = leg.missed ? proc::PROCEDURE_MISSED : proc::PROCEDURE_APPROACH;
    }

    if(!procedure.transitionLegs.isEmpty())
    {
      procedure.mapType |= proc::PROCEDURE_TRANSITION;
      for(MapProcedureLeg& leg : procedure.transitionLegs)
        leg.mapType = proc::PROCEDURE_TRANSITION;
    }
  }
}

/* Create proceed to runway entry based on information in given leg and the runway end information
 *  in the given legs */
proc::MapProcedureLeg ProcedureQuery::createRunwayLeg(const proc::MapProcedureLeg& leg,
                                                      const proc::MapProcedureLegs& legs)
{
  proc::MapProcedureLeg rwleg;
  rwleg.approachId = legs.ref.approachId;
  rwleg.transitionId = legs.ref.transitionId;
  rwleg.approachId = legs.ref.approachId;
  rwleg.navId = leg.navId;

  // Use a generated id base on the previous leg id
  rwleg.legId = RUNWAY_LEG_ID_BASE + leg.legId;

  rwleg.altRestriction.descriptor = proc::MapAltRestriction::AT;
  rwleg.altRestriction.alt2 = 0.f;
  // geometry is populated later
  rwleg.fixType = "R";
  rwleg.fixIdent = "RW" + legs.runwayEnd.name;
  rwleg.fixPos = legs.runwayEnd.position;
  rwleg.time = 0.f;
  rwleg.theta = 0.f;
  rwleg.rho = 0.f;
  rwleg.magvar = leg.magvar;
  rwleg.distance = meterToNm(rwleg.line.lengthMeter());
  rwleg.course = normalizeCourse(rwleg.line.angleDeg() - rwleg.magvar);
  rwleg.navaids.runwayEnds.append(legs.runwayEnd);
  rwleg.missed = rwleg.flyover = rwleg.trueCourse = rwleg.intercept = rwleg.disabled = false;

  return rwleg;
}

/* Create start of procedure entry based on information in given leg */
proc::MapProcedureLeg ProcedureQuery::createStartLeg(const proc::MapProcedureLeg& leg,
                                                     const proc::MapProcedureLegs& legs)
{
  proc::MapProcedureLeg sleg;
  sleg.approachId = legs.ref.approachId;
  sleg.transitionId = legs.ref.transitionId;
  sleg.approachId = legs.ref.approachId;

  // Use a generated id base on the previous leg id
  sleg.legId = START_LEG_ID_BASE + leg.legId;
  sleg.displayText.append(tr("Start"));
  // geometry is populated later

  sleg.fixPos = leg.fixPos;
  sleg.fixIdent = leg.fixIdent;
  sleg.fixRegion = leg.fixRegion;
  sleg.fixType = leg.fixType;
  sleg.navId = leg.navId;
  sleg.navaids = leg.navaids;

  // Correct distance is calculated in the RouteLeg to get a transition from route to procedure
  sleg.time = 0.f;
  sleg.theta = 0.f;
  sleg.rho = 0.f;
  sleg.magvar = leg.magvar;
  sleg.distance = 0.f;
  sleg.course = 0.f;
  sleg.missed = sleg.flyover = sleg.trueCourse = sleg.intercept = sleg.disabled = false;

  return sleg;
}
