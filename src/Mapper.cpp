#include "Mapper.h"
#include <nabo/nabo.h>
#include <fstream>
#include <chrono>

Mapper::Mapper(std::string icpConfigFilePath, std::string inputFiltersConfigFilePath, std::string inputFiltersWorldFilePath, std::string mapPostFiltersConfigFilePath, std::string mapUpdateCondition,
			   float mapUpdateOverlap, float mapUpdateDelay, float mapUpdateDistance, float minDistNewPoint, float sensorMaxRange,
			   float priorDynamic, float thresholdDynamic, float beamHalfAngle, float epsilonA, float epsilonD, float alpha, float beta,
			   bool is3D, bool isOnline, bool computeProbDynamic, bool isMapping):
		transformation(PM::get().TransformationRegistrar.create("RigidTransformation")),
		icpConfigFilePath(icpConfigFilePath),
		inputFiltersConfigFilePath(inputFiltersConfigFilePath),
		inputFiltersWorldFilePath(inputFiltersWorldFilePath),
		mapPostFiltersConfigFilePath(mapPostFiltersConfigFilePath),
		mapUpdateCondition(mapUpdateCondition),
		mapUpdateOverlap(mapUpdateOverlap),
		mapUpdateDelay(mapUpdateDelay),
		mapUpdateDistance(mapUpdateDistance),
		minDistNewPoint(minDistNewPoint),
		sensorMaxRange(sensorMaxRange),
		priorDynamic(priorDynamic),
		thresholdDynamic(thresholdDynamic),
		beamHalfAngle(beamHalfAngle),
		epsilonA(epsilonA),
		epsilonD(epsilonD),
		alpha(alpha),
		beta(beta),
		is3D(is3D),
		isOnline(isOnline),
		computeProbDynamic(computeProbDynamic),
		isMapping(isMapping),
		newMapAvailable(false),
		isMapEmpty(true)
{
	loadYamlConfig();
	
	PM::Parameters radiusFilterParams;
	radiusFilterParams["dim"] = "-1";
	radiusFilterParams["dist"] = std::to_string(sensorMaxRange);
	radiusFilterParams["removeInside"] = "0";
	radiusFilter = PM::get().DataPointsFilterRegistrar.create("DistanceLimitDataPointsFilter", radiusFilterParams);
	
	int homogeneousDim = is3D ? 4 : 3;
	sensorPose = PM::Matrix::Identity(homogeneousDim, homogeneousDim);
}

void Mapper::loadYamlConfig()
{
	if(!icpConfigFilePath.empty())
	{
		std::ifstream ifs(icpConfigFilePath.c_str());
		icp.loadFromYaml(ifs);
		ifs.close();
	}
	else
	{
		icp.setDefault();
	}
	
	if(!inputFiltersConfigFilePath.empty())
	{
		std::ifstream ifs(inputFiltersConfigFilePath.c_str());
		inputFilters = PM::DataPointsFilters(ifs);
		ifs.close();
	}
	
	if(!inputFiltersWorldFilePath.empty())
	{
		std::ifstream ifs(inputFiltersWorldFilePath.c_str());
		inputFiltersWorld = PM::DataPointsFilters(ifs);
		ifs.close();
	}
	
	if(!mapPostFiltersConfigFilePath.empty())
	{
		std::ifstream ifs(mapPostFiltersConfigFilePath.c_str());
		mapPostFilters = PM::DataPointsFilters(ifs);
		ifs.close();
	}
}

void Mapper::processInput(PM::DataPoints& inputInSensorFrame, const PM::TransformationParameters& estimatedSensorPose,
						  const std::chrono::time_point<std::chrono::steady_clock>& timeStamp)
{

    PM::DataPoints inputInMapFrame = transformation->compute(inputInSensorFrame, estimatedSensorPose);
    inputFiltersWorld.apply(inputInMapFrame);

	radiusFilter->inPlaceFilter(inputInSensorFrame);
	inputFilters.apply(inputInSensorFrame);

	if(isMapEmpty)
	{
		sensorPose = estimatedSensorPose;
		
		updateMap(inputInMapFrame, timeStamp);
	}
	else
	{
		icpMapLock.lock();
		PM::TransformationParameters correction = icp(inputInMapFrame);
		icpMapLock.unlock();
		
		sensorPose = correction * estimatedSensorPose;
		
		if(shouldUpdateMap(timeStamp, sensorPose, icp.errorMinimizer->getOverlap()))
		{
			updateMap(transformation->compute(inputInMapFrame, correction), timeStamp);
		}
	}
}

bool Mapper::shouldUpdateMap(const std::chrono::time_point<std::chrono::steady_clock>& currentTime, const PM::TransformationParameters& currentSensorPose,
							 const float& currentOverlap)
{
	if(!isMapping)
	{
		return false;
	}
	
	if(isOnline)
	{
		// if previous map is not done building
		if(mapBuilderFuture.valid() && mapBuilderFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		{
			return false;
		}
	}
	
	if(mapUpdateCondition == "overlap")
	{
		return currentOverlap < mapUpdateOverlap;
	}
	else if(mapUpdateCondition == "delay")
	{
		return (currentTime - lastTimeMapWasUpdated) > std::chrono::duration<float>(mapUpdateDelay);
	}
	else
	{
		int euclideanDim = is3D ? 3 : 2;
		PM::Vector lastSensorLocation = lastSensorPoseWhereMapWasUpdated.topRightCorner(euclideanDim, 1);
		PM::Vector currentSensorLocation = currentSensorPose.topRightCorner(euclideanDim, 1);
		return std::abs((currentSensorLocation - lastSensorLocation).norm()) > mapUpdateDistance;
	}
}

void Mapper::updateMap(const PM::DataPoints& currentInput, const std::chrono::time_point<std::chrono::steady_clock>& timeStamp)
{
	lastTimeMapWasUpdated = timeStamp;
	lastSensorPoseWhereMapWasUpdated = sensorPose;
	
	if(isOnline && !isMapEmpty)
	{
		mapBuilderFuture = std::async(&Mapper::buildMap, this, currentInput, getMap(), sensorPose);
	}
	else
	{
		buildMap(currentInput, getMap(), sensorPose);
	}
}

void Mapper::buildMap(PM::DataPoints currentInput, PM::DataPoints currentMap, PM::TransformationParameters currentSensorPose)
{
	if(computeProbDynamic)
	{
		currentInput.addDescriptor("probabilityDynamic", PM::Matrix::Constant(1, currentInput.features.cols(), priorDynamic));
	}
	
	if(isMapEmpty)
	{
		currentMap = currentInput;
	}
	else
	{
		if(computeProbDynamic)
		{
			computeProbabilityOfPointsBeingDynamic(currentInput, currentMap, currentSensorPose);
		}
		
		PM::DataPoints inputPointsToKeep = retrievePointsFurtherThanMinDistNewPoint(currentInput, currentMap, currentSensorPose);
		currentMap.concatenate(inputPointsToKeep);
	}
	
	PM::DataPoints mapInSensorFrame = transformation->compute(currentMap, currentSensorPose.inverse());
	mapPostFilters.apply(mapInSensorFrame);
	currentMap = transformation->compute(mapInSensorFrame, currentSensorPose);
	
	setMap(currentMap, currentSensorPose);
}

void Mapper::computeProbabilityOfPointsBeingDynamic(const PM::DataPoints& currentInput, PM::DataPoints& currentMap,
													const PM::TransformationParameters& currentSensorPose)
{
	typedef Nabo::NearestNeighbourSearch<T> NNS;
	const float eps = 0.0001;
	
	PM::DataPoints currentInputInSensorFrame = transformation->compute(currentInput, currentSensorPose.inverse());
	
	PM::Matrix currentInputInSensorFrameRadii;
	PM::Matrix currentInputInSensorFrameAngles;
	convertToSphericalCoordinates(currentInputInSensorFrame, currentInputInSensorFrameRadii, currentInputInSensorFrameAngles);
	
	PM::DataPoints cutMapInSensorFrame = transformation->compute(currentMap, currentSensorPose.inverse());
	PM::Matrix globalId(1, currentMap.getNbPoints());
	int nbPointsCutMap = 0;
	for(int i = 0; i < currentMap.getNbPoints(); i++)
	{
		if(cutMapInSensorFrame.features.col(i).head(cutMapInSensorFrame.getEuclideanDim()).norm() < sensorMaxRange)
		{
			cutMapInSensorFrame.setColFrom(nbPointsCutMap, cutMapInSensorFrame, i);
			globalId(0, nbPointsCutMap) = i;
			nbPointsCutMap++;
		}
	}
	cutMapInSensorFrame.conservativeResize(nbPointsCutMap);
	
	PM::Matrix cutMapInSensorFrameRadii;
	PM::Matrix cutMapInSensorFrameAngles;
	convertToSphericalCoordinates(cutMapInSensorFrame, cutMapInSensorFrameRadii, cutMapInSensorFrameAngles);
	
	std::shared_ptr<NNS> nns = std::shared_ptr<NNS>(NNS::create(currentInputInSensorFrameAngles));
	PM::Matches::Dists dists(1, cutMapInSensorFrame.getNbPoints());
	PM::Matches::Ids ids(1, cutMapInSensorFrame.getNbPoints());
	nns->knn(cutMapInSensorFrameAngles, ids, dists, 1, 0, NNS::ALLOW_SELF_MATCH, 2 * beamHalfAngle);
	
	PM::DataPoints::View viewOnProbabilityDynamic = currentMap.getDescriptorViewByName("probabilityDynamic");
	PM::DataPoints::View viewOnMapNormals = cutMapInSensorFrame.getDescriptorViewByName("normals");
	for(int i = 0; i < cutMapInSensorFrame.getNbPoints(); i++)
	{
		if(dists(i) != std::numeric_limits<float>::infinity())
		{
			const int readingPointId = ids(0, i);
			const int mapPointId = globalId(0, i);
			
			const Eigen::VectorXf readingPoint = currentInputInSensorFrame.features.col(readingPointId).head(currentInputInSensorFrame.getEuclideanDim());
			const Eigen::VectorXf mapPoint = cutMapInSensorFrame.features.col(i).head(cutMapInSensorFrame.getEuclideanDim());
			const float delta = (readingPoint - mapPoint).norm();
			const float d_max = epsilonA * readingPoint.norm();
			
			const Eigen::VectorXf mapPointNormal = viewOnMapNormals.col(i);
			
			const float w_v = eps + (1. - eps) * fabs(mapPointNormal.dot(mapPoint.normalized()));
			const float w_d1 = eps + (1. - eps) * (1. - sqrt(dists(i)) / (2 * beamHalfAngle));
			
			const float offset = delta - epsilonD;
			float w_d2 = 1.;
			if(delta < epsilonD || mapPoint.norm() > readingPoint.norm())
			{
				w_d2 = eps;
			}
			else
			{
				if(offset < d_max)
				{
					w_d2 = eps + (1 - eps) * offset / d_max;
				}
			}
			
			float w_p2 = eps;
			if(delta < epsilonD)
			{
				w_p2 = 1;
			}
			else
			{
				if(offset < d_max)
				{
					w_p2 = eps + (1. - eps) * (1. - offset / d_max);
				}
			}
			
			if((readingPoint.norm() + epsilonD + d_max) >= mapPoint.norm())
			{
				const float lastDyn = viewOnProbabilityDynamic(0, mapPointId);
				
				const float c1 = (1 - (w_v * w_d1));
				const float c2 = w_v * w_d1;
				
				float probDynamic;
				float probStatic;
				if(lastDyn < thresholdDynamic)
				{
					probDynamic = c1 * lastDyn + c2 * w_d2 * ((1 - alpha) * (1 - lastDyn) + beta * lastDyn);
					probStatic = c1 * (1 - lastDyn) + c2 * w_p2 * (alpha * (1 - lastDyn) + (1 - beta) * lastDyn);
				}
				else
				{
					probDynamic = 1 - eps;
					probStatic = eps;
				}
				
				viewOnProbabilityDynamic(0, mapPointId) = probDynamic / (probDynamic + probStatic);
			}
		}
	}
}

PM::DataPoints Mapper::retrievePointsFurtherThanMinDistNewPoint(const PM::DataPoints& currentInput, const PM::DataPoints& currentMap,
																const PM::TransformationParameters& currentSensorPose)
{
	typedef Nabo::NearestNeighbourSearch<T> NNS;
	
	PM::DataPoints cutMapInSensorFrame = transformation->compute(currentMap, currentSensorPose.inverse());
	radiusFilter->inPlaceFilter(cutMapInSensorFrame);
	PM::DataPoints cutMap = transformation->compute(cutMapInSensorFrame, currentSensorPose);
	
	PM::Matches matches(PM::Matches::Dists(1, currentInput.getNbPoints()), PM::Matches::Ids(1, currentInput.getNbPoints()));
	std::shared_ptr<NNS> nns = std::shared_ptr<NNS>(NNS::create(cutMap.features, cutMap.features.rows() - 1, NNS::KDTREE_LINEAR_HEAP, NNS::TOUCH_STATISTICS));
	
	nns->knn(currentInput.features, matches.ids, matches.dists, 1, 0);
	
	int goodPointCount = 0;
	PM::DataPoints goodPoints(currentInput.createSimilarEmpty());
	for(int i = 0; i < currentInput.getNbPoints(); ++i)
	{
		if(matches.dists(i) >= std::pow(minDistNewPoint, 2))
		{
			goodPoints.setColFrom(goodPointCount, currentInput, i);
			goodPointCount++;
		}
	}
	goodPoints.conservativeResize(goodPointCount);
	
	return goodPoints;
}

void Mapper::convertToSphericalCoordinates(const PM::DataPoints& points, PM::Matrix& radii, PM::Matrix& angles)
{
	radii = points.features.topRows(points.getEuclideanDim()).colwise().norm();
	angles = PM::Matrix(2, points.getNbPoints());
	
	for(int i = 0; i < points.getNbPoints(); i++)
	{
		angles(0, i) = 0;
		if(is3D)
		{
			const float ratio = points.features(2, i) / radii(0, i);
			angles(0, i) = asin(ratio);
		}
		angles(1, i) = atan2(points.features(1, i), points.features(0, i));
	}
}

PM::DataPoints Mapper::getMap()
{
	std::lock_guard<std::mutex> lock(mapLock);
	return map;
}

void Mapper::setMap(const PM::DataPoints& newMap, const PM::TransformationParameters& newSensorPose)
{
	if(computeProbDynamic && !newMap.descriptorExists("normals"))
	{
		throw std::runtime_error("compute prob dynamic is set to true, but field normals does not exist for map points.");
	}
	
	PM::DataPoints cutMapInSensorFrame = transformation->compute(newMap, newSensorPose.inverse());
	radiusFilter->inPlaceFilter(cutMapInSensorFrame);
	PM::DataPoints cutMap = transformation->compute(cutMapInSensorFrame, newSensorPose);
	
	icpMapLock.lock();
	icp.setMap(cutMap);
	icpMapLock.unlock();
	
	mapLock.lock();
	map = newMap;
	newMapAvailable = true;
	mapLock.unlock();
	
	isMapEmpty = newMap.getNbPoints() == 0;
}

bool Mapper::getNewMap(PM::DataPoints& mapOut)
{
	bool mapReturned = false;
	
	mapLock.lock();
	if(newMapAvailable)
	{
		mapOut = map;
		newMapAvailable = false;
		mapReturned = true;
	}
	mapLock.unlock();
	
	return mapReturned;
}

const PM::TransformationParameters& Mapper::getSensorPose()
{
	return sensorPose;
}
