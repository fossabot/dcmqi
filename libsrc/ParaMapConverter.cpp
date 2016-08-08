#include <itkImageDuplicator.h>
#include "ParaMapConverter.h"


using namespace std;

namespace dcmqi {

  DcmDataset* ParaMapConverter::itkimage2paramap(const ImageType::Pointer &parametricMapImage, DcmDataset* dcmDataset,
                                         const string &metaData) {

    MinMaxCalculatorType::Pointer calculator = MinMaxCalculatorType::New();
    calculator->SetImage(parametricMapImage);
    calculator->Compute();

    JSONParametricMapMetaInformationHandler metaInfo(metaData);
    metaInfo.read();

    metaInfo.setFirstValueMapped(calculator->GetMinimum());
    metaInfo.setLastValueMapped(calculator->GetMaximum());

    IODEnhGeneralEquipmentModule::EquipmentInfo eq = getEnhEquipmentInfo();
    ContentIdentificationMacro contentID = createContentIdentificationInformation(metaInfo);
    CHECK_COND(contentID.setInstanceNumber(metaInfo.getInstanceNumber().c_str()));

    // TODO: following should maybe be moved to meta info
    OFString imageFlavor = "VOLUME";
    OFString pixContrast = "NONE";
    if(metaInfo.metaInfoRoot.isMember("DerivedPixelContrast")){
      pixContrast = metaInfo.metaInfoRoot["DerivedPixelContrast"].asCString();
    }

    // TODO: initialize modality from the source / add to schema?
    OFString modality = "MR";

    ImageType::SizeType inputSize = parametricMapImage->GetBufferedRegion().GetSize();
    cout << "Input image size: " << inputSize << endl;

    OFvariant<OFCondition,DPMParametricMapIOD> obj =
        DPMParametricMapIOD::create<IODFloatingPointImagePixelModule>(modality, metaInfo.getSeriesNumber().c_str(),
                                                                      metaInfo.getInstanceNumber().c_str(),
                                                                      inputSize[0], inputSize[1], eq, contentID,
                                                                      imageFlavor, pixContrast, DPMTypes::CQ_RESEARCH);
    if (OFCondition* pCondition = OFget<OFCondition>(&obj))
      return NULL;

    DPMParametricMapIOD* pMapDoc = OFget<DPMParametricMapIOD>(&obj);

    if (dcmDataset != NULL)
      CHECK_COND(pMapDoc->import(*dcmDataset, OFTrue, OFTrue, OFFalse, OFTrue));

    /* Initialize dimension module */
    char dimUID[128];
    dcmGenerateUniqueIdentifier(dimUID, QIICR_UID_ROOT);
    IODMultiframeDimensionModule &mfdim = pMapDoc->getIODMultiframeDimensionModule();
    OFCondition result = mfdim.addDimensionIndex(DCM_ImagePositionPatient, dimUID,
                                                 DCM_RealWorldValueMappingSequence, "Frame position");

    // Shared FGs: PixelMeasuresSequence
    {
      FGPixelMeasures *pixmsr = new FGPixelMeasures();

      ImageType::SpacingType labelSpacing = parametricMapImage->GetSpacing();
      ostringstream spacingSStream;
      spacingSStream << scientific << labelSpacing[0] << "\\" << labelSpacing[1];
      CHECK_COND(pixmsr->setPixelSpacing(spacingSStream.str().c_str()));

      spacingSStream.clear(); spacingSStream.str("");
      spacingSStream << scientific << labelSpacing[2];
      CHECK_COND(pixmsr->setSpacingBetweenSlices(spacingSStream.str().c_str()));
      CHECK_COND(pixmsr->setSliceThickness(spacingSStream.str().c_str()));
      CHECK_COND(pMapDoc->addForAllFrames(*pixmsr));
    }

    // Shared FGs: PlaneOrientationPatientSequence
    {
      OFString imageOrientationPatientStr;

      ImageType::DirectionType labelDirMatrix = parametricMapImage->GetDirection();

      cout << "Directions: " << labelDirMatrix << endl;

      FGPlaneOrientationPatient *planor =
          FGPlaneOrientationPatient::createMinimal(
              Helper::floatToStrScientific(labelDirMatrix[0][0]).c_str(),
              Helper::floatToStrScientific(labelDirMatrix[1][0]).c_str(),
              Helper::floatToStrScientific(labelDirMatrix[2][0]).c_str(),
              Helper::floatToStrScientific(labelDirMatrix[0][1]).c_str(),
              Helper::floatToStrScientific(labelDirMatrix[1][1]).c_str(),
              Helper::floatToStrScientific(labelDirMatrix[2][1]).c_str());

      //CHECK_COND(planor->setImageOrientationPatient(imageOrientationPatientStr));
      CHECK_COND(pMapDoc->addForAllFrames(*planor));
    }

    FGFrameAnatomy frameAnaFG;
    frameAnaFG.setLaterality(FGFrameAnatomy::LATERALITY_UNPAIRED);
    if(metaInfo.metaInfoRoot.isMember("AnatomicRegionCode")){
      frameAnaFG.getAnatomy().getAnatomicRegion().set(
          metaInfo.metaInfoRoot["AnatomicRegionCode"]["CodeValue"].asCString(),
          metaInfo.metaInfoRoot["AnatomicRegionCode"]["CodingSchemeDesignator"].asCString(),
          metaInfo.metaInfoRoot["AnatomicRegionCode"]["CodeMeaning"].asCString());
    } else {
      frameAnaFG.getAnatomy().getAnatomicRegion().set("T-D0050", "SRT", "Tissue");
    }
    CHECK_COND(pMapDoc->addForAllFrames(frameAnaFG));

    FGIdentityPixelValueTransformation idTransFG;
    // Rescale Intercept, Rescale Slope, Rescale Type are missing here
    CHECK_COND(pMapDoc->addForAllFrames(idTransFG));

    FGParametricMapFrameType frameTypeFG;
    std::string frameTypeStr = "DERIVED\\PRIMARY\\VOLUME\\";
    if(metaInfo.metaInfoRoot.isMember("DerivedPixelContrast")){
      frameTypeStr = frameTypeStr + metaInfo.metaInfoRoot["DerivedPixelContrast"].asCString();
    } else {
      frameTypeStr = frameTypeStr + "NONE";
    }
    frameTypeFG.setFrameType(frameTypeStr.c_str());
    CHECK_COND(pMapDoc->addForAllFrames(frameTypeFG));

    FGRealWorldValueMapping rwvmFG;
    FGRealWorldValueMapping::RWVMItem* realWorldValueMappingItem = new FGRealWorldValueMapping::RWVMItem();
    if (!realWorldValueMappingItem )
    {
      return NULL;
    }

    realWorldValueMappingItem->setRealWorldValueSlope(atof(metaInfo.getRealWorldValueSlope().c_str()));
    realWorldValueMappingItem->setRealWorldValueIntercept(atof(metaInfo.getRealWorldValueIntercept().c_str()));

    realWorldValueMappingItem->setRealWorldValueFirstValueMappeSigned(metaInfo.getFirstValueMapped());
    realWorldValueMappingItem->setRealWorldValueLastValueMappedSigned(metaInfo.getLastValueMapped());

    CodeSequenceMacro* measurementUnitCode = metaInfo.getMeasurementUnitsCode();
    if (measurementUnitCode != NULL) {
      realWorldValueMappingItem->getMeasurementUnitsCode().set(metaInfo.getCodeSequenceValue(measurementUnitCode).c_str(),
                                                               metaInfo.getCodeSequenceDesignator(measurementUnitCode).c_str(),
                                                               metaInfo.getCodeSequenceMeaning(measurementUnitCode).c_str());
    }

    // TODO: LutExplanation and LUTLabel should be added as Metainformation
    realWorldValueMappingItem->setLUTExplanation(metaInfo.metaInfoRoot["MeasurementUnitsCode"]["CodeMeaning"].asCString());
    realWorldValueMappingItem->setLUTLabel(metaInfo.metaInfoRoot["MeasurementUnitsCode"]["CodeValue"].asCString());
    ContentItemMacro* quantity = new ContentItemMacro;
    CodeSequenceMacro* qCodeName = new CodeSequenceMacro("G-C1C6", "SRT", "Quantity");
    CodeSequenceMacro* qSpec = new CodeSequenceMacro(
      metaInfo.metaInfoRoot["QuantityValueCode"]["CodeValue"].asCString(),
      metaInfo.metaInfoRoot["QuantityValueCode"]["CodingSchemeDesignator"].asCString(),
      metaInfo.metaInfoRoot["QuantityValueCode"]["CodeMeaning"].asCString());

    if (!quantity || !qSpec || !qCodeName)
    {
      return NULL;
    }

    quantity->getEntireConceptNameCodeSequence().push_back(qCodeName);
    quantity->getEntireConceptCodeSequence().push_back(qSpec);
    realWorldValueMappingItem->getEntireQuantityDefinitionSequence().push_back(quantity);
    quantity->setValueType(ContentItemMacro::VT_CODE);
    rwvmFG.getRealWorldValueMapping().push_back(realWorldValueMappingItem);
    CHECK_COND(pMapDoc->addForAllFrames(rwvmFG));

    for (unsigned long f = 0; result.good() && (f < inputSize[2]); f++) {
      result = addFrame(*pMapDoc, parametricMapImage, metaInfo, f);
    }

  {
    string bodyPartAssigned = metaInfo.getBodyPartExamined();
    if(dcmDataset != NULL && bodyPartAssigned.empty()) {
      OFString bodyPartStr;
      if(dcmDataset->findAndGetOFString(DCM_BodyPartExamined, bodyPartStr).good()) {
        if (!bodyPartStr.empty())
          bodyPartAssigned = bodyPartStr.c_str();
      }
    }
    if(!bodyPartAssigned.empty())
      pMapDoc->getIODGeneralSeriesModule().setBodyPartExamined(bodyPartAssigned.c_str());
  }

  // SeriesDate/Time should be of when parametric map was taken; initialize to when it was saved
  {
    OFString contentDate, contentTime;
    DcmDate::getCurrentDate(contentDate);
    DcmTime::getCurrentTime(contentTime);

    pMapDoc->getSeries().setSeriesDate(contentDate.c_str());
    pMapDoc->getSeries().setSeriesTime(contentTime.c_str());
    pMapDoc->getGeneralImage().setContentDate(contentDate.c_str());
    pMapDoc->getGeneralImage().setContentTime(contentTime.c_str());
  }
  pMapDoc->getSeries().setSeriesDescription(metaInfo.getSeriesDescription().c_str());
  pMapDoc->getSeries().setSeriesNumber(metaInfo.getSeriesNumber().c_str());

  DcmDataset* output = new DcmDataset();
  CHECK_COND(pMapDoc->writeDataset(*output));
  return output;
  }

  int ParaMapConverter::paramap2itkimage(const string &inputFileName, const string &outputDirName) {

    DcmRLEDecoderRegistration::registerCodecs();

    dcemfinfLogger.setLogLevel(dcmtk::log4cplus::OFF_LOG_LEVEL);

    DcmFileFormat segFF;
    DcmDataset *pmapDataset = NULL;
    if(segFF.loadFile(inputFileName.c_str()).good()){
      pmapDataset = segFF.getDataset();
    } else {
      cerr << "Failed to read input " << endl;
      return EXIT_FAILURE;
    }

    OFvariant<OFCondition,DPMParametricMapIOD*> result = DPMParametricMapIOD::loadFile(inputFileName.c_str());
    if (OFCondition* pCondition = OFget<OFCondition>(&result)) {
      return EXIT_FAILURE;
    }

    DPMParametricMapIOD* pMapDoc = *OFget<DPMParametricMapIOD*>(&result);

    // Directions
    FGInterface &fgInterface = pMapDoc->getFunctionalGroups();
    ImageType::DirectionType direction;
    if(getImageDirections(fgInterface, direction)){
      cerr << "Failed to get image directions" << endl;
      return EXIT_FAILURE;
    }

    // Spacing and origin
    double computedSliceSpacing, computedVolumeExtent;
    vnl_vector<double> sliceDirection(3);
    sliceDirection[0] = direction[0][2];
    sliceDirection[1] = direction[1][2];
    sliceDirection[2] = direction[2][2];

    ImageType::PointType imageOrigin;
    if(computeVolumeExtent(fgInterface, sliceDirection, imageOrigin, computedSliceSpacing, computedVolumeExtent)){
      cerr << "Failed to compute origin and/or slice spacing!" << endl;
      return EXIT_FAILURE;
    }

    ImageType::SpacingType imageSpacing;
    imageSpacing.Fill(0);
    if(getDeclaredImageSpacing(fgInterface, imageSpacing)){
      cerr << "Failed to get image spacing from DICOM!" << endl;
      return EXIT_FAILURE;
    }

    const double tolerance = 1e-5;
    if(!imageSpacing[2]){
      imageSpacing[2] = computedSliceSpacing;
    } else if(fabs(imageSpacing[2]-computedSliceSpacing)>tolerance){
      cerr << "WARNING: Declared slice spacing is significantly different from the one declared in DICOM!" <<
           " Declared = " << imageSpacing[2] << " Computed = " << computedSliceSpacing << endl;
    }

    // Region size
    ImageType::SizeType imageSize;
    {
      OFString str;

      if(pmapDataset->findAndGetOFString(DCM_Rows, str).good())
        imageSize[1] = atoi(str.c_str());
      if(pmapDataset->findAndGetOFString(DCM_Columns, str).good())
        imageSize[0] = atoi(str.c_str());
    }
    imageSize[2] = fgInterface.getNumberOfFrames();

    ImageType::RegionType imageRegion;
    imageRegion.SetSize(imageSize);
    ImageType::Pointer segImage = ImageType::New();
    segImage->SetRegions(imageRegion);
    segImage->SetOrigin(imageOrigin);
    segImage->SetSpacing(imageSpacing);
    segImage->SetDirection(direction);
    segImage->Allocate();
    segImage->FillBuffer(0);

    typedef itk::ImageFileWriter<ImageType> WriterType;
    stringstream imageFileNameSStream;
    imageFileNameSStream << outputDirName << "/" << "pmap.nrrd";

    DPMParametricMapIOD::FramesType obj = pMapDoc->getFrames();
    if (OFCondition* pCondition = OFget<OFCondition>(&obj)) {
      return EXIT_FAILURE;
    }

    DPMParametricMapIOD::Frames<PixelType> frames = *OFget<DPMParametricMapIOD::Frames<PixelType> >(&obj);

    for(int frameId=0;frameId<fgInterface.getNumberOfFrames();frameId++){

      PixelType *frame = frames.getFrame(frameId);

      bool isPerFrame;

      FGPlanePosPatient *planposfg =
          OFstatic_cast(FGPlanePosPatient*,fgInterface.get(frameId, DcmFGTypes::EFG_PLANEPOSPATIENT, isPerFrame));
      assert(planposfg);

      FGFrameContent *fracon =
          OFstatic_cast(FGFrameContent*,fgInterface.get(frameId, DcmFGTypes::EFG_FRAMECONTENT, isPerFrame));
      assert(fracon);

      // populate meta information needed for Slicer ScalarVolumeNode initialization
      {
      }

      ImageType::IndexType index;
      // initialize slice with the frame content
      for(int row=0;row<imageSize[1];row++){
        index[1] = row;
        index[2] = frameId;
        for(int col=0;col<imageSize[0];col++){
          unsigned pixelPosition = row*imageSize[0] + col;
          index[0] = col;
          segImage->SetPixel(index, frame[pixelPosition]);
        }
      }
    }

    WriterType::Pointer writer = WriterType::New();
    writer->SetFileName(imageFileNameSStream.str().c_str());
    writer->SetInput(segImage);
    writer->SetUseCompression(1);
    writer->Update();

    return EXIT_SUCCESS;
  }

  OFCondition ParaMapConverter::addFrame(DPMParametricMapIOD &map, const ImageType::Pointer &parametricMapImage,
                                         const JSONParametricMapMetaInformationHandler &metaInfo,
                                         const unsigned long frameNo)
  {
    ImageType::RegionType sliceRegion;
    ImageType::IndexType sliceIndex;
    ImageType::SizeType inputSize = parametricMapImage->GetBufferedRegion().GetSize();

    sliceIndex[0] = 0;
    sliceIndex[1] = 0;
    sliceIndex[2] = frameNo;

    inputSize[2] = 1;

    sliceRegion.SetIndex(sliceIndex);
    sliceRegion.SetSize(inputSize);

    const unsigned frameSize = inputSize[0] * inputSize[1];

    OFVector<IODFloatingPointImagePixelModule::value_type> data(frameSize);

    itk::ImageRegionConstIteratorWithIndex<ImageType> sliceIterator(parametricMapImage, sliceRegion);

    unsigned framePixelCnt = 0;
    for(sliceIterator.GoToBegin();!sliceIterator.IsAtEnd(); ++sliceIterator, ++framePixelCnt){
      data[framePixelCnt] = sliceIterator.Get();
      ImageType::IndexType idx = sliceIterator.GetIndex();
//      cout << framePixelCnt << " " << idx[1] << "," << idx[0] << endl;
    }

    OFVector<FGBase*> groups;
    OFunique_ptr<FGPlanePosPatient> fgPlanePos(new FGPlanePosPatient);
    OFunique_ptr<FGFrameContent > fgFracon(new FGFrameContent);
    if (!fgPlanePos  || !fgFracon)
    {
      return EC_MemoryExhausted;
    }

    // Plane Position
    OFStringStream ss;
    ss << frameNo;
    OFSTRINGSTREAM_GETOFSTRING(ss, framestr) // convert number to string
    fgPlanePos->setImagePositionPatient("0", "0", framestr);

    // Frame Content
    OFCondition result = fgFracon->setDimensionIndexValues(frameNo+1 /* value within dimension */, 0 /* first dimension */);

    // Add frame with related groups
    if (result.good())
    {
      groups.push_back(fgPlanePos.get());
      groups.push_back(fgFracon.get());
      groups.push_back(fgPlanePos.get());
      DPMParametricMapIOD::FramesType frames = map.getFrames();
      result = OFget<DPMParametricMapIOD::Frames<PixelType> >(&frames)->addFrame(&*data.begin(), frameSize, groups);
    }
    return result;
  }
}
